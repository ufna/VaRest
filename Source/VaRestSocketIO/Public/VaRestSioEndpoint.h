// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "VaRestSocketEndpoint.h"
#include "VaRestSioTypes.h"
#include "VaRestSioEndpoint.generated.h"

class FVaRestSioSocket;

UENUM(BlueprintType)
enum class EVaRestSioEventDirection : uint8 { In, Out, OutWithAck, InWithAck };

USTRUCT(BlueprintType)
struct VARESTSOCKETIO_API FVaRestSioEventDeclaration
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Event") FString Event;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Event") EVaRestSioEventDirection Direction = EVaRestSioEventDirection::In;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Event") FVaRestTypeRef PayloadType;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Event") EVaRestSioArgs ArgsMode = EVaRestSioArgs::First;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Reply", meta=(EditCondition="Direction == EVaRestSioEventDirection::OutWithAck")) FVaRestTypeRef ReplyType;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Reply", meta=(EditCondition="Direction == EVaRestSioEventDirection::OutWithAck")) EVaRestSioArgs ReplyArgsMode = EVaRestSioArgs::First;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Reply", meta=(EditCondition="Direction == EVaRestSioEventDirection::OutWithAck")) bool bErrorFirstReply = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Event", meta=(MultiLine="true")) FString Description;
};

USTRUCT(BlueprintType)
struct VARESTSOCKETIO_API FVaRestSioNamespaceDeclaration
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Namespace") FString Name = TEXT("/");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Namespace") bool bConnectOnStart = true;
	/** Static CONNECT object. Validation never includes this text in diagnostics. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Authentication", meta=(MultiLine="true")) FString AuthPayload;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Authentication") bool bSendSession = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Events") TArray<FVaRestSioEventDeclaration> Events;
};

/** Socket.IO connection and namespace declarations; Path is the Engine.IO path, never a namespace. */
UCLASS(BlueprintType, EditInlineNew, DefaultToInstanced, meta=(DisplayName="Socket.IO Socket"))
class VARESTSOCKETIO_API UVaRestSioEndpoint : public UVaRestSocketEndpointBase
{
	GENERATED_BODY()
public:
	UVaRestSioEndpoint();
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Namespaces") FString DefaultNamespace = TEXT("/");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Namespaces") TArray<FVaRestSioNamespaceDeclaration> Namespaces;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Authentication") FVaRestSioAuthPlacement AuthPlacement;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Recovery") bool bRecovery = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Acks", meta=(ClampMin="0")) double AckTimeout = 10.0;
	// The shared base owns the transport part of Limits; these complete the protocol limits.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Limits", meta=(EditCondition="bOverrideLimits", ClampMin="0", ClampMax="4095")) int32 MaxAttachments = 10;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Limits", meta=(EditCondition="bOverrideLimits", ClampMin="1")) int64 MaxPacketBytes = 16 * 1024 * 1024;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Limits", meta=(EditCondition="bOverrideLimits", ClampMin="1")) int64 MaxOutboundFrameBytes = 16 * 1024 * 1024;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Limits", meta=(EditCondition="bOverrideLimits")) bool bOverrideFallbackPayloadLimit = false;
	virtual FName GetProtocolTag() const override { return TEXT("SocketIO"); }
	virtual void Validate(FVaRestApiValidationContext& Context) const override;
	FVaRestSioLimits GetSioLimits() const;
	/** Called on GT after the native protocol has been installed and before publication. */
	bool ConfigureSio(FVaRestSioSocket& Socket) const;
};
