#include "WallhackTrackedRig.h"
#include "Math/RotationMatrix.h"

bool FWallhackRigAlignment::Align(FVector RadarCentre,const FTransform& ControllerAim,FString& Error)
{
    Error.Reset();
    if(IsReady()){Error=TEXT("RESET BEFORE RECALIBRATING");return false;}
    if(RadarCentre.ContainsNaN()||ControllerAim.ContainsNaN()||!ControllerAim.GetRotation().IsNormalized()
        ||!ControllerAim.GetScale3D().Equals(FVector::OneVector))
    {Error=TEXT("CALIBRATION POSE INVALID");return false;}
    if(FVector::Dist(RadarCentre,ControllerAim.GetLocation())>1.)
    {Error=TEXT("RADAR MUST BE WITHIN 1 M OF LEFT CONTROLLER");return false;}
    const FVector Forward=ControllerAim.GetUnitAxis(EAxis::X);
    if(FMath::Abs(Forward.Z)>.9)
    {Error=TEXT("POINT RIG FORWARD / NOT STRAIGHT UP OR DOWN");return false;}
    const FTransform Sensor(FRotationMatrix::MakeFromXZ(Forward,FVector::UpVector).ToQuat(),RadarCentre);
    return SetMount(Sensor.GetRelativeTransform(ControllerAim),Error);
}

bool FWallhackRigAlignment::SetMount(const FTransform& InMount,FString& Error)
{
    Error.Reset();
    if(InMount.ContainsNaN()||!InMount.GetRotation().IsNormalized()
        ||!InMount.GetScale3D().Equals(FVector::OneVector)||InMount.GetLocation().Size()>1.)
    {Error=TEXT("CALIBRATION OFFSET INVALID");return false;}
    Mount=InMount;bReady=true;return true;
}

void FWallhackRigCalibrationCapture::ClearWindow()
{
    FirstAt=LastAt=-1;Count=0;SumOffset=FVector::ZeroVector;SumRotation=FQuat(0,0,0,0);
    bWindowEstimated=false;
}
void FWallhackRigCalibrationCapture::Reset()
{
    ClearWindow();bActive=bReady=bResultEstimated=false;Result=FTransform::Identity;
}
void FWallhackRigCalibrationCapture::LoseTracking()
{
    ClearWindow();
    // A completed local mount offset survives a brief tracking gap, but no
    // samples from before a gap can contribute to an unfinished capture.
}
float FWallhackRigCalibrationCapture::Progress() const
{
    return bReady?1.f:Count>0?FMath::Clamp(float((LastAt-FirstAt)/.65),0.f,1.f):0.f;
}
bool FWallhackRigCalibrationCapture::Observe(double At,const FTransform& LeftAim,FVector RadarCentre,FString& Status,bool bEstimated)
{
    if(!bActive)return bReady;
    FWallhackRigAlignment Sample;
    if(!FMath::IsFinite(At)||!Sample.Align(RadarCentre,LeftAim,Status))
    {ClearWindow();if(Status.IsEmpty())Status=TEXT("CALIBRATION SAMPLE INVALID");return false;}
    const FTransform& Mount=Sample.GetMount();
    const bool bGap=Count>0&&(At<=LastAt||At-LastAt>.1);
    const bool bMoved=Count>0&&(FVector::Dist(LeftAim.GetLocation(),FirstAim.GetLocation())>.02
        ||LeftAim.GetRotation().AngularDistance(FirstAim.GetRotation())>FMath::DegreesToRadians(3.f)
        ||FVector::Dist(Mount.GetLocation(),FirstMount.GetLocation())>.015
        ||Mount.GetRotation().AngularDistance(FirstMount.GetRotation())>FMath::DegreesToRadians(2.f));
    if(bGap||bMoved)ClearWindow();
    if(Count==0){FirstAt=At;FirstAim=LeftAim;FirstMount=Mount;}
    LastAt=At;++Count;SumOffset+=Mount.GetLocation();
    bWindowEstimated|=bEstimated;
    FQuat Rotation=Mount.GetRotation();
    if((Rotation|FirstMount.GetRotation())<0)Rotation=Rotation*-1;
    SumRotation=SumRotation+Rotation;
    Status=bMoved?TEXT("KEEP RIG AND WHITE PROBE STILL"):TEXT("CAPTURING ORIGIN / HOLD STILL");
    if(At-FirstAt<.65||Count<16)return false;
    Result=FTransform(SumRotation.GetNormalized(),SumOffset/Count);
    bActive=false;bReady=true;bResultEstimated=bWindowEstimated;
    Status=TEXT("CHECK ORIGIN AND FORWARD / TRIGGER TO ACCEPT");return true;
}

FWallhackRigGuideGeometry BuildWallhackRigCalibrationGuides(const FTransform& SensorMetres)
{
    FWallhackRigGuideGeometry G;
    if(SensorMetres.ContainsNaN()||!SensorMetres.GetRotation().IsNormalized()
        ||!SensorMetres.GetScale3D().Equals(FVector::OneVector))return G;
    auto Stroke=[&](FVector A,FVector B,float Width,FLinearColor Color)
    {
        const FVector Direction=(B-A).GetSafeNormal();
        FVector U,V;Direction.FindBestAxisVectors(U,V);
        // Crossed ribbons give the physical guide area in both eye views.
        for(const FVector Axis:{U,V})
        {
            const FVector Half=Axis*(Width*.5f);const int32 Base=G.Vertices.Num();
            for(const FVector P:{A-Half,A+Half,B+Half,B-Half})
            {G.Vertices.Add(SensorMetres.TransformPosition(P));G.Colors.Add(Color);}
            G.Indices.Append({Base,Base+1,Base+2,Base,Base+2,Base+3});
        }
    };
    const FLinearColor White(.86f,.92f,.9f,.85f),Mint(.44f,.95f,.73f,.75f);
    Stroke({-.035,0,0},{.035,0,0},.003f,White);
    Stroke({0,-.035,0},{0,.035,0},.003f,White);
    Stroke({0,0,-.035},{0,0,.035},.003f,White);
    for(int32 I=0;I<32;++I)
    {
        const double A=I*2*UE_PI/32,B=(I+1)*2*UE_PI/32;
        Stroke({0,.045*FMath::Cos(A),.045*FMath::Sin(A)},
            {0,.045*FMath::Cos(B),.045*FMath::Sin(B)},.002f,White.CopyWithNewOpacity(.35f));
    }
    Stroke({.05,0,0},{.50,0,0},.004f,Mint);
    Stroke({.40,-.05,0},{.50,0,0},.005f,Mint);
    Stroke({.40,.05,0},{.50,0,0},.005f,Mint);
    Stroke({.40,0,-.05},{.50,0,0},.005f,Mint);
    Stroke({.40,0,.05},{.50,0,0},.005f,Mint);
    Stroke({0,0,.05},{0,0,.13},.003f,White);
    return G;
}

void FWallhackRigHistory::Add(double At,const FTransform& Pose,bool bEstimated)
{
    if(!FMath::IsFinite(At)||Pose.ContainsNaN()){Reset();return;}
    if(!Samples.IsEmpty()&&(At<=Samples.Last().At||At-Samples.Last().At>.1))Reset();
    Samples.Add({At,Pose,bEstimated});
    while(Samples.Num()>256||(!Samples.IsEmpty()&&At-Samples[0].At>2))Samples.RemoveAt(0);
}
bool FWallhackRigHistory::Sample(double At,FTransform& Out,bool* bEstimated) const
{
    if(bEstimated)*bEstimated=false;
    if(!FMath::IsFinite(At)||Samples.IsEmpty()||At<Samples[0].At||At>Samples.Last().At)return false;
    for(int32 I=Samples.Num()-1;I>=0;--I)
    {
        if(At<Samples[I].At)continue;
        Out=Samples[I].Pose;
        if(bEstimated)*bEstimated=Samples[I].bEstimated;
        if(I+1<Samples.Num()&&At>Samples[I].At)
        {
            Out.Blend(Samples[I].Pose,Samples[I+1].Pose,(At-Samples[I].At)/(Samples[I+1].At-Samples[I].At));
            if(bEstimated)*bEstimated|=Samples[I+1].bEstimated;
        }
        return true;
    }
    return false;
}
