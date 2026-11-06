// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "VaRestInspectorViewModel.h"
#include "Widgets/SLeafWidget.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"

/** Draws only cached native values. Unknown phases have no invented duration. */
class SVaRestInspectorWaterfall final : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SVaRestInspectorWaterfall) {} SLATE_ARGUMENT(TWeakPtr<FVaRestInspectorViewModel>, Model) SLATE_END_ARGS()
	void Construct(const FArguments& Args)
	{
		Model = Args._Model;
		Legend = FText::FromString(TEXT("Gap (hatched) · Auth · Queue · Prepare · Send · Wait · Receive"));
		SetToolTipText(FText::FromString(TEXT("Known phases only. Detailed durations and unknown phases are listed below.")));
	}
	FVector2D ComputeDesiredSize(float) const override
	{
		const auto Value = Model.Pin(); return FVector2D(440, 30 + (Value ? Value->Details().Waterfall.Num() : 0) * 28);
	}
	int32 OnPaint(const FPaintArgs&, const FGeometry& Geometry, const FSlateRect&, FSlateWindowElementList& Out,
		int32 Layer, const FWidgetStyle& Style, bool Enabled) const override
	{
		const auto Value = Model.Pin(); if (!Value) return Layer;
		const auto& Data = Value->Details();
		const FLinearColor Colors[] = {{.35f,.35f,.35f,1},{.65f,.35f,.85f,1},{.85f,.65f,.2f,1},{.3f,.7f,.7f,1},{.2f,.5f,.9f,1},{.9f,.5f,.2f,1},{.3f,.75f,.4f,1}};
		const auto Effect = Enabled ? ESlateDrawEffect::None : ESlateDrawEffect::DisabledEffect;
		const float Width = FMath::Max(1.f, static_cast<float>(Geometry.GetLocalSize().X) - 98.f);
		const double Span = FMath::Max(1e-6, Data.WaterfallEnd - Data.WaterfallBegin);
		const auto Font = FCoreStyle::GetDefaultFontStyle("Regular", 9);
		FSlateDrawElement::MakeText(Out, Layer, Geometry.ToPaintGeometry(FVector2f(Width + 90, 24), FSlateLayoutTransform(FVector2f(0, 0))), Legend, Font, Effect, Style.GetColorAndOpacityTint());
		for (int32 I = 0; I < Data.Waterfall.Num(); ++I)
		{
			const auto& Lane = Data.Waterfall[I]; const float Y = 28.f + I * 28.f;
			FSlateDrawElement::MakeText(Out, Layer, Geometry.ToPaintGeometry(FVector2f(94, 24), FSlateLayoutTransform(FVector2f(0, Y))), Lane.Label, Font, Effect, Style.GetColorAndOpacityTint());
			for (const auto& Part : Lane.Segments)
			{
				const float X = 96.f + float((Part.Start - Data.WaterfallBegin) / Span) * Width;
				const float W = FMath::Max(1.f, float(Part.Seconds / Span) * Width);
				FSlateDrawElement::MakeBox(Out, Layer, Geometry.ToPaintGeometry(FVector2f(W, 18), FSlateLayoutTransform(FVector2f(X, Y))), FCoreStyle::Get().GetBrush("WhiteBrush"), Effect, Colors[Part.Phase]);
				if (Part.Phase == 0) for (float H = 0; H < W; H += 7)
					FSlateDrawElement::MakeBox(Out, Layer + 1, Geometry.ToPaintGeometry(FVector2f(FMath::Min(2.f, W - H), 18), FSlateLayoutTransform(FVector2f(X + H, Y))), FCoreStyle::Get().GetBrush("WhiteBrush"), Effect, FLinearColor(.12f,.12f,.12f,1));
			}
		}
		return Layer + 1;
	}
private:
	TWeakPtr<FVaRestInspectorViewModel> Model;
	FText Legend;
};
