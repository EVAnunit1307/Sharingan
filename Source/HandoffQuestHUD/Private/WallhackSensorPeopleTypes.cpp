#include "WallhackSensorPeopleTypes.h"
#include "Json.h"

namespace
{
bool Number(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, double& Out, double Min, double Max)
{
    return O.IsValid() && O->TryGetNumberField(Key, Out) && FMath::IsFinite(Out) && Out >= Min && Out <= Max;
}
bool Id(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, int32& Out)
{
    double V;
    if (!Number(O, Key, V, 0, MAX_int32) || V != double(int32(V))) return false;
    Out = int32(V); return true;
}
bool Position(const TSharedPtr<FJsonObject>& O, FVector2D& Out)
{
    double Right, Forward;
    if (!Number(O, TEXT("right_m"), Right, -1000, 1000) || !Number(O, TEXT("forward_m"), Forward, -1000, 1000)) return false;
    Out = FVector2D(Right, Forward); return true;
}
bool String(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, FString& Out)
{
    return O->TryGetStringField(Key, Out) && !Out.IsEmpty() && Out.Len() <= 256;
}
}

void FWallhackSensorPeopleStream::Reset()
{
    Last = {}; RelaySession.Reset(); Sequence = -1; CameraExpiry = {};
    RadarGeneration = RadarHighWater = -1; RadarExpiries.Reset();
}

double FWallhackSensorPeopleStream::CameraDeadline(int32 Generation, int32 Frame, double AgeMs, double Now)
{
    const double Candidate = Now + .750 - AgeMs / 1000.;
    if (Generation < CameraExpiry.Generation || (Generation == CameraExpiry.Generation && Frame < CameraExpiry.Frame)) return -1;
    if (Generation != CameraExpiry.Generation || Frame != CameraExpiry.Frame) CameraExpiry = {Generation, Frame, Candidate};
    else CameraExpiry.At = FMath::Min(CameraExpiry.At, Candidate);
    return CameraExpiry.At;
}

double FWallhackSensorPeopleStream::RadarDeadline(int32 Generation, int32 Frame, double AgeMs, double Now)
{
    if (Generation < RadarGeneration) return -1;
    if (Generation > RadarGeneration) { RadarExpiries.Reset(); RadarHighWater = -1; RadarGeneration = Generation; }
    if (Frame < RadarHighWater - 128) return -1;
    RadarHighWater = FMath::Max(RadarHighWater, Frame);
    for (auto It = RadarExpiries.CreateIterator(); It; ++It) if (It.Key() < RadarHighWater - 128) It.RemoveCurrent();
    const double Candidate = Now + .500 - AgeMs / 1000.;
    if (double* Existing = RadarExpiries.Find(Frame)) { *Existing = FMath::Min(*Existing, Candidate); return *Existing; }
    RadarExpiries.Add(Frame, Candidate); return Candidate;
}

bool FWallhackSensorPeopleStream::Ingest(const FString& Json, double Now)
{
    if (!FMath::IsFinite(Now) || Json.Len() > 1024 * 1024) return false;
    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid()) return false;
    const TSharedPtr<FJsonObject>* Extension = nullptr;
    if (!Root->TryGetObjectField(TEXT("spatial_people"), Extension) || !Extension || !Extension->IsValid()) return false;
    const auto& E = *Extension;
    int32 Version;
    FString Session, Source, Reference, Units, Coordinates, Origin;
    double Seq;
    if (!Id(E, TEXT("version"), Version) || Version != 1
        || !String(Root, TEXT("relay_session_id"), Session)
        || !Number(Root, TEXT("relay_sequence"), Seq, 0, 9007199254740991.) || Seq != double(int64(Seq))
        || !String(E, TEXT("source_session_id"), Source) || !String(E, TEXT("reference_id"), Reference)
        || !String(E, TEXT("units"), Units) || Units != TEXT("m")
        || !String(E, TEXT("coordinate_frame"), Coordinates) || Coordinates != TEXT("sensor_reference_2d")
        || !String(E, TEXT("reference_origin"), Origin)
        || (Origin != TEXT("radar") && Origin != TEXT("configured_drone_reference"))) return false;
    if (Session == RelaySession && int64(Seq) <= Sequence) return false;
    const TArray<TSharedPtr<FJsonValue>>* People = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Dots = nullptr;
    if (!E->TryGetArrayField(TEXT("people"), People) || !People || People->Num() > 8
        || !E->TryGetArrayField(TEXT("radar_targets"), Dots) || !Dots || Dots->Num() > 32) return false;

    // Work on a candidate stream so a malformed tail never renews valid samples.
    FWallhackSensorPeopleStream Candidate = *this;
    const FString Key = Session + TEXT("/") + Source + TEXT("/") + Reference;
    if (Candidate.RelaySession != Session || Candidate.Last.RegistrationKey != Key) Candidate.Reset();
    FWallhackSensorPeopleFrame Frame;
    Frame.RegistrationKey = Key;
    E->TryGetStringField(TEXT("status"), Frame.Status);
    Root->TryGetBoolField(TEXT("is_replay"), Frame.bReplay);
    Id(E, TEXT("unpositioned_people"), Frame.Unpositioned);
    int32 SourceCameraFrame = -1, SourceCameraGeneration = -1;
    double SourceCameraAge;
    const bool bHasCameraFrame = Id(E, TEXT("camera_frame_id"), SourceCameraFrame)
        && Id(E, TEXT("camera_generation"), SourceCameraGeneration)
        && Number(E, TEXT("camera_age_ms"), SourceCameraAge, 0, 1.e9);
    if (bHasCameraFrame)
    {
        // Empty frames advance the high-water mark too, so missing people cannot
        // return via an older sample wrapped in a newer relay packet.
        if (SourceCameraGeneration < Candidate.CameraExpiry.Generation
            || (SourceCameraGeneration == Candidate.CameraExpiry.Generation
                && SourceCameraFrame < Candidate.CameraExpiry.Frame)) return false;
        Candidate.CameraDeadline(SourceCameraGeneration, SourceCameraFrame, SourceCameraAge, Now);
    }
    else if (!People->IsEmpty()) return false;
    TSet<int32> Seen;
    for (const auto& Value : *People)
    {
        if (!Value.IsValid() || Value->Type != EJson::Object) return false;
        auto O = Value->AsObject();
        FWallhackSensorPerson P;
        int32 CameraFrame;
        double Confidence, CameraAge;
        FString Kind;
        if (!Id(O, TEXT("camera_id"), P.Id) || Seen.Contains(P.Id)
            || !Id(O, TEXT("camera_generation"), P.CameraGeneration)
            || !Id(O, TEXT("camera_frame_id"), CameraFrame) || !Position(O, P.Position)
            || CameraFrame != SourceCameraFrame || P.CameraGeneration != SourceCameraGeneration
            || !Number(O, TEXT("confidence"), Confidence, 0, 1)
            || !Number(O, TEXT("camera_age_ms"), CameraAge, 0, 1.e9)
            || !String(O, TEXT("position_source"), Kind)) return false;
        Seen.Add(P.Id);
        P.Confidence = float(Confidence);
        P.CameraExpires = Candidate.CameraDeadline(P.CameraGeneration, CameraFrame, CameraAge, Now);
        const TSharedPtr<FJsonObject>* Fallback = nullptr;
        if (O->TryGetObjectField(TEXT("fallback_position"), Fallback) && Fallback && Fallback->IsValid())
        {
            if (!Position(*Fallback, P.Fallback)) return false;
            P.bHasFallback = true;
        }
        if (Kind == TEXT("radar_matched"))
        {
            int32 Generation, RadarFrame; double Age;
            if (!Id(O, TEXT("radar_id"), P.RadarId) || !Id(O, TEXT("radar_generation"), Generation)
                || !Id(O, TEXT("radar_frame_id"), RadarFrame) || !Number(O, TEXT("radar_age_ms"), Age, 0, 1.e9)) return false;
            P.bRadar = true;
            P.RadarExpires = Candidate.RadarDeadline(Generation, RadarFrame, Age, Now);
        }
        else if (Kind != TEXT("camera_estimate")) return false;
        Frame.People.Add(P);
    }
    for (const auto& Value : *Dots)
    {
        if (!Value.IsValid() || Value->Type != EJson::Object) return false;
        auto O = Value->AsObject(); FWallhackSensorDot Dot;
        int32 Generation, RadarFrame; double Age;
        if (!Id(O, TEXT("id"), Dot.Id) || !Position(O, Dot.Position)
            || !Id(O, TEXT("generation"), Generation) || !Id(O, TEXT("frame_id"), RadarFrame)
            || !Number(O, TEXT("age_ms"), Age, 0, 1.e9)) return false;
        Dot.Expires = Candidate.RadarDeadline(Generation, RadarFrame, Age, Now);
        Frame.Radar.Add(Dot);
    }
    Candidate.Last = MoveTemp(Frame); Candidate.RelaySession = Session; Candidate.Sequence = int64(Seq);
    *this = MoveTemp(Candidate);
    return true;
}

FWallhackSensorPeopleFrame FWallhackSensorPeopleStream::GetFrame(double Now) const
{
    FWallhackSensorPeopleFrame Frame = Last;
    Frame.People.RemoveAll([Now](FWallhackSensorPerson& P)
    {
        if (!FMath::IsFinite(Now) || Now > P.CameraExpires) return true;
        if (P.bRadar && Now > P.RadarExpires)
        {
            if (!P.bHasFallback) return true;
            P.Position = P.Fallback; P.bRadar = false; P.RadarId = INDEX_NONE;
        }
        return false;
    });
    Frame.Radar.RemoveAll([Now](const FWallhackSensorDot& D) { return !FMath::IsFinite(Now) || Now > D.Expires; });
    return Frame;
}
