#include "WallhackSensorPeopleTypes.h"
#include "Json.h"

void FWallhackSensorPositionInterpolator::Reset() { Tracks.Reset(); Time = -1; }
void FWallhackSensorPositionInterpolator::BeginFrame(double Now)
{
    // A rendering/tracking pause must not animate from an obsolete location.
    if (!FMath::IsFinite(Now) || Now < Time || (Time >= 0 && Now - Time > .25)) Reset();
    Time = FMath::IsFinite(Now) ? Now : -1;
    for (auto& Entry : Tracks) Entry.Value.bSeen = false;
}
FVector2D FWallhackSensorPositionInterpolator::Evaluate(const FTrack& Track) const
{
    return FMath::Lerp(Track.From, Track.To, FMath::Clamp((Time - Track.StartedAt) / .12, 0., 1.));
}
FVector2D FWallhackSensorPositionInterpolator::Sample(const FString& Key, FVector2D Position)
{
    if (Time < 0 || Position.ContainsNaN()) return Position;
    auto* Track = Tracks.Find(Key);
    if (!Track)
    {
        Tracks.Add(Key, {Position, Position, Time, true});
        return Position;
    }
    Track->bSeen = true;
    const FVector2D Current = Evaluate(*Track);
    if (!Position.Equals(Track->To, 1.e-6))
    {
        // Identity reassignment/outliers must not sweep a body across a room.
        Track->From = FVector2D::Distance(Current, Position) > 1.5 ? Position : Current;
        Track->To = Position;
        Track->StartedAt = Time;
    }
    return Evaluate(*Track);
}
void FWallhackSensorPositionInterpolator::EndFrame()
{
    for (auto It = Tracks.CreateIterator(); It; ++It) if (!It.Value().bSeen) It.RemoveCurrent();
}

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
    TrackExpiries.Reset();
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
    double SourceCameraAge=0;
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
            P.RadarGeneration = Generation;
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
        Dot.Generation = Generation;
        Frame.Radar.Add(Dot);
    }
    if(E->HasField(TEXT("rig_pose_valid"))&&!E->TryGetBoolField(TEXT("rig_pose_valid"),Frame.bRigPoseValid))return false;
    if(E->HasField(TEXT("rig_motion_mode")))
    {
        if(!E->TryGetStringField(TEXT("rig_motion_mode"),Frame.RigMotionMode)
            ||(Frame.RigMotionMode!=TEXT("stationary")&&Frame.RigMotionMode!=TEXT("untracked")
                &&Frame.RigMotionMode!=TEXT("left_controller")))return false;
    }
    if (E->HasField(TEXT("tracks_version")))
    {
        int32 TrackVersion;
        const TArray<TSharedPtr<FJsonValue>>* Tracks=nullptr;
        if(!Id(E,TEXT("tracks_version"),TrackVersion)||TrackVersion!=1
            ||!E->TryGetArrayField(TEXT("tracks"),Tracks)||!Tracks||Tracks->Num()>8)return false;
        Frame.bHasTracks=true;
        TSet<int32> TrackIds;
        for(auto It=Candidate.TrackExpiries.CreateIterator();It;++It)if(It.Value()<Now-1)It.RemoveCurrent();
        for(const auto& Value:*Tracks)
        {
            if(!Value.IsValid()||Value->Type!=EJson::Object)return false;
            const auto O=Value->AsObject();FWallhackTrackedPerson P;
            double VX,VY,Height,Facing,Remaining;FString Sample;
            if(!Id(O,TEXT("id"),P.Id)||TrackIds.Contains(P.Id)||!Position(O,P.Position)
                ||!Number(O,TEXT("velocity_right_mps"),VX,-20,20)||!Number(O,TEXT("velocity_forward_mps"),VY,-20,20)
                ||!Number(O,TEXT("height_m"),Height,.5,2.8)||!Number(O,TEXT("facing_deg"),Facing,-720,720)
                ||!Number(O,TEXT("valid_for_ms"),Remaining,0,750)||!String(O,TEXT("sample_key"),Sample)
                ||!String(O,TEXT("position_source"),P.Source))return false;
            if(P.Source!=TEXT("radar_matched")&&P.Source!=TEXT("radar_only")&&P.Source!=TEXT("camera_estimate"))return false;
            if(P.Source!=TEXT("camera_estimate")&&Remaining>500)return false;
            TrackIds.Add(P.Id);P.Velocity={VX,VY};P.Height=Height;P.Facing=Facing;
            O->TryGetStringField(TEXT("height_source"),P.HeightSource);
            O->TryGetStringField(TEXT("facing_source"),P.FacingSource);
            O->TryGetStringField(TEXT("person_evidence"),P.PersonEvidence);
            double PoseYaw=0;Number(O,TEXT("pose_yaw_deg"),PoseYaw,-720,720);P.PoseYaw=PoseYaw;
            const FString ExpiryKey=FString::FromInt(P.Id)+TEXT("/")+Sample;
            const double Deadline=Now+Remaining/1000.;
            if(auto* Old=Candidate.TrackExpiries.Find(ExpiryKey))*Old=FMath::Min(*Old,Deadline);
            else Candidate.TrackExpiries.Add(ExpiryKey,Deadline);
            P.Expires=Candidate.TrackExpiries[ExpiryKey];
            P.SampleKey=Sample;
            P.ObservedAt=P.Expires-(P.Source==TEXT("camera_estimate")?.750:.500);
            const TSharedPtr<FJsonObject>* Pose=nullptr;
            if(O->TryGetObjectField(TEXT("pose"),Pose)&&Pose&&Pose->IsValid())
            {
                const TArray<TSharedPtr<FJsonValue>>* Joints=nullptr;double Age,Capture;
                if(!Number(*Pose,TEXT("age_ms"),Age,0,1.e9)||!Number(*Pose,TEXT("capture_ms"),Capture,0,1.e15)
                    ||!(*Pose)->TryGetArrayField(TEXT("joints"),Joints)||!Joints||Joints->Num()!=33)return false;
                const FString PoseKey=FString::Printf(TEXT("P/%d/%.0f"),P.Id,Capture);
                const double PoseDeadline=Now+.350-Age/1000.;
                if(auto* Old=Candidate.TrackExpiries.Find(PoseKey))*Old=FMath::Min(*Old,PoseDeadline);
                else Candidate.TrackExpiries.Add(PoseKey,PoseDeadline);
                P.PoseExpires=Candidate.TrackExpiries[PoseKey];
                P.PoseObservedAt=P.PoseExpires-.350;
                for(const auto& Joint:*Joints)
                {
                    const TArray<TSharedPtr<FJsonValue>>* V=nullptr;
                    if(!Joint.IsValid()||!Joint->TryGetArray(V)||!V||V->Num()!=4)return false;
                    double A[4];
                    for(int32 I=0;I<4;++I)if(!(*V)[I]->TryGetNumber(A[I])||!FMath::IsFinite(A[I])||FMath::Abs(A[I])>4)return false;
                    if(A[3]<0||A[3]>1)return false;
                    P.Joints.Add(FVector(A[2],A[0],A[1]));P.JointQuality.Add(A[3]);
                }
            }
            Frame.Tracks.Add(MoveTemp(P));
        }
        if(Candidate.TrackExpiries.Num()>512)return false;
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
    Frame.Tracks.RemoveAll([Now](FWallhackTrackedPerson& P)
    {
        if(!FMath::IsFinite(Now)||Now>P.Expires)return true;
        if(Now>P.PoseExpires){P.Joints.Reset();P.JointQuality.Reset();}
        return false;
    });
    return Frame;
}
