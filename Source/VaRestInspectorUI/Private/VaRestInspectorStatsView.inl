// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
// GTK presentation values; no capture views or snapshot owners are retained.
namespace
{
const TCHAR* StatsColumnNames[] = {TEXT("Type"),TEXT("Scope"),TEXT("Client"),TEXT("Key / socket"),TEXT("Completed / closed"),TEXT("Errors"),TEXT("Error %"),TEXT("Attempts"),TEXT("Retries / reconnects"),TEXT("Auth replays"),TEXT("Cache hits"),TEXT("Bytes up"),TEXT("Bytes down"),TEXT("p50 ms"),TEXT("p95 ms"),TEXT("p99 ms"),TEXT("Max ms"),TEXT("Frames in"),TEXT("Frames out"),TEXT("Drops"),TEXT("Avg RTT ms"),TEXT("Partial"),TEXT("Incomplete")};
static_assert(UE_ARRAY_COUNT(StatsColumnNames)==FVaRestInspectorStatsRowModel::ColumnCount);
void StatsTextCell(FVaRestInspectorStatsRowModel& Row,int32 I,FString Text)
{ Row.Columns[I].Kind=1; Row.Columns[I].Text=FText::FromString(MoveTemp(Text)); }
void StatsCountCell(FVaRestInspectorStatsRowModel& Row,int32 I,uint64 Value)
{ Row.Columns[I].Kind=2; Row.Columns[I].Count=Value; Row.Columns[I].Text=FText::FromString(LexToString(Value)); }
void StatsNumberCell(FVaRestInspectorStatsRowModel& Row,int32 I,double Value)
{
 if (!FMath::IsFinite(Value)) return;
 Row.Columns[I].Kind=3; Row.Columns[I].Number=Value; Row.Columns[I].Text=FText::FromString(FString::Printf(TEXT("%.3f"),Value));
}
void StatsQuantileCell(FVaRestInspectorStatsRowModel& Row,int32 I,const FVaRestInspectorQuantile& Q)
{
 if (!Q.bHasValue) return;
 if (Q.Kind==EVaRestInspectorQuantileKind::Zero) { StatsNumberCell(Row,I,0); return; }
 if (Q.Kind==EVaRestInspectorQuantileKind::LogInterval)
 { StatsNumberCell(Row,I,Q.Value*1000); Row.Columns[I].Text=FText::FromString(FString::Printf(TEXT("~%.3f [%.3f,%.3f)"),Q.Value*1000,Q.Lower*1000,Q.Upper*1000)); }
 else StatsTextCell(Row,I,TEXT("outside range / saturated"));
}
}
void FVaRestInspectorViewModel::AddStatsPresentation(TSharedRef<FVaRestInspectorStatsRowModel> Row)
{
 if (Row->Columns[1].Kind==2)
  for (const auto& Option:ScopeOptions) if (!Option->bAll && Option->Id==Row->Columns[1].Count) { Row->Columns[1].Text=Option->Label; break; }
 int64 Bytes=sizeof(FVaRestInspectorStatsRowModel)+128;
 for (const auto& Cell:Row->Columns) Bytes+=int64(Cell.Text.ToString().Len()+1)*sizeof(TCHAR);
 if (StatsTable.Num()>=1536 || Bytes>8*1024*1024-StatsPresentationBytes) { bStatsPresentationOmitted=true; return; }
 StatsPresentationBytes+=Bytes; StatsTable.Add(Row);
}
void FVaRestInspectorViewModel::AddStatsRow(const FVaRestInspectorHttpStats& G)
{
 auto Row=MakeShared<FVaRestInspectorStatsRowModel>(); auto& R=*Row;
 StatsTextCell(R,0,G.bOverflow ? TEXT("HTTP overflow") : TEXT("HTTP"));
 if (!G.bOverflow) { StatsCountCell(R,1,G.ScopeId); StatsCountCell(R,2,G.ClientId); }
 StatsTextCell(R,3,G.bOverflow ? FString(TEXT("Unattributed overflow")) : G.Endpoint.IsNone() ? G.Key.Left(256) : G.Endpoint.ToString());
 StatsCountCell(R,4,G.Completed); StatsCountCell(R,5,G.Error); if (G.Completed) StatsNumberCell(R,6,double(G.Error)*100/double(G.Completed));
 StatsCountCell(R,7,G.Dispatches); StatsCountCell(R,8,G.Retries); StatsCountCell(R,9,G.AuthReplays); StatsCountCell(R,10,G.CacheHit);
 StatsCountCell(R,11,G.WireBytesUp); StatsCountCell(R,12,G.WireBytesDown);
 StatsQuantileCell(R,13,G.Quantile(1,2)); StatsQuantileCell(R,14,G.Quantile(95,100)); StatsQuantileCell(R,15,G.Quantile(99,100));
 if (G.DurationCount) StatsNumberCell(R,16,G.DurationMax*1000);
 StatsCountCell(R,19,G.Dropped); StatsCountCell(R,21,G.Partial); StatsCountCell(R,22,G.Incomplete);
 if (G.bSaturated || G.bDurationCountsSaturated) R.Columns[0].Text=FText::FromString(TEXT("HTTP · saturated"));
 AddStatsPresentation(Row);
}
void FVaRestInspectorViewModel::AddStatsRow(const FVaRestInspectorSocketStats& G)
{
 auto Row=MakeShared<FVaRestInspectorStatsRowModel>(); auto& R=*Row;
 StatsTextCell(R,0,G.bOverflow ? TEXT("Socket overflow") : G.Protocol==TEXT("SocketIO") || G.Protocol==TEXT("sio") ? TEXT("Socket.IO") : TEXT("WebSocket"));
 if (G.bContextKnown) StatsCountCell(R,1,G.ScopeId);
 StatsTextCell(R,3,G.bOverflow ? FString(TEXT("Unattributed overflow")) : FString::Printf(TEXT("%s (#%llu)%s"),*G.Name.ToString(),G.SocketId,G.bReleased ? TEXT(" · released") : TEXT("")));
 StatsCountCell(R,4,G.Closed); StatsCountCell(R,5,G.Failures); if (G.Closed) StatsNumberCell(R,6,double(G.Failures)*100/double(G.Closed));
 StatsCountCell(R,7,G.Attempts); StatsCountCell(R,8,G.Reconnects); StatsCountCell(R,11,G.BytesUp); StatsCountCell(R,12,G.BytesDown);
 StatsCountCell(R,17,G.FramesIn); StatsCountCell(R,18,G.FramesOut); StatsCountCell(R,19,G.Drops);
 if (G.RttCount) StatsNumberCell(R,20,G.RttSum/double(G.RttCount)*1000);
 StatsCountCell(R,21,G.bPartial ? 1 : 0);
 if (G.bSaturated) R.Columns[0].Text=FText::FromString(R.Columns[0].Text.ToString()+TEXT(" · saturated"));
 AddStatsPresentation(Row);
}
void FVaRestInspectorViewModel::SortStats(int32 Column,bool Ascending)
{
 check(IsInGameThread()); StatsSortColumn=FMath::Clamp(Column,0,FVaRestInspectorStatsRowModel::ColumnCount-1); bStatsAscending=Ascending; SortStatsRows();
}
void FVaRestInspectorViewModel::SortStatsRows()
{
 StatsTable.StableSort([this](const auto& L,const auto& R)
 {
  const auto Compare=[](const FVaRestInspectorStatsCell& A,const FVaRestInspectorStatsCell& B)
  {
   if (A.Kind==2 && B.Kind==2) return A.Count<B.Count ? -1 : A.Count>B.Count ? 1 : 0;
   if (A.Kind==3 && B.Kind==3) return A.Number<B.Number ? -1 : A.Number>B.Number ? 1 : 0;
   return A.Text.ToString().Compare(B.Text.ToString(),ESearchCase::CaseSensitive);
  };
  const auto& A=L->Columns[StatsSortColumn]; const auto& B=R->Columns[StatsSortColumn];
  if ((A.Kind==0)!=(B.Kind==0)) return B.Kind==0;
  const int32 Primary=Compare(A,B); if (Primary) return bStatsAscending ? Primary<0 : Primary>0;
  for (int32 I:{1,0,2,3}) { const int32 Tie=Compare(L->Columns[I],R->Columns[I]); if (Tie) return Tie<0; }
  return false;
 });
}
FString FVaRestInspectorViewModel::StatsTsv() const
{
 check(IsInGameThread()); FString Out;
 for (const TCHAR* Name:StatsColumnNames) { if (!Out.IsEmpty()) Out+=TEXT("\t"); Out+=Name; } Out+=TEXT("\n");
 for (const auto& Row:StatsTable)
 {
  FString Line;
  for (int32 I=0;I<FVaRestInspectorStatsRowModel::ColumnCount;++I)
  {
   FString Cell=Row->Columns[I].Kind ? Row->Columns[I].Text.ToString() : FString(TEXT("unknown / n/a"));
   Cell.ReplaceInline(TEXT("\t"),TEXT(" ")); Cell.ReplaceInline(TEXT("\r"),TEXT(" ")); Cell.ReplaceInline(TEXT("\n"),TEXT(" "));
   if (I) Line+=TEXT("\t"); Line+=Cell;
  }
  if (Line.Len()>4*1024*1024-Out.Len()) { Out+=TEXT("(table copy truncated)\n"); break; }
  Out+=Line+TEXT("\n");
 }
 if (bStatsPresentationOmitted) Out+=TEXT("(presentation limit omitted groups)\n");
 return Out;
}
