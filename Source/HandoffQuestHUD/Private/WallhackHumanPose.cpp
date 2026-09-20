#include "WallhackHumanPose.h"

TMap<FName,FTransform> WallhackHumanPose::Solve(const FWallhackPersonPose& P,
    const TMap<FName,FTransform>& Ref,double Now)
{
    auto Out=Ref;
    auto Location=[&](FName N){const auto* T=Ref.Find(N);return T?T->GetLocation():FVector::ZeroVector;};
    auto Length=[&](FName A,FName B){return FVector::Dist(Location(A),Location(B));};
    const bool Camera=P.bCameraPose&&P.Joints.Num()==33&&P.JointQuality.Num()==33;
    auto Reliable=[&](int A,int B){return Camera&&FMath::Min(P.JointQuality[A],P.JointQuality[B])>=.5f;};
    auto Direction=[&](int A,int B,FVector Fallback)
    {
        if(!Reliable(A,B))return Fallback.GetSafeNormal();
        const FVector D=P.Joints[B]-P.Joints[A];
        return D.SizeSquared()>.0001?D.GetSafeNormal():Fallback.GetSafeNormal();
    };
    auto Set=[&](FName N,FVector Position,FVector ReferenceDirection,FVector Desired)
    {
        const auto* R=Ref.Find(N);if(!R)return;
        const FQuat Swing=FQuat::FindBetweenNormals(ReferenceDirection.GetSafeNormal(),Desired.GetSafeNormal());
        Out[N]=FTransform((Swing*R->GetRotation()).GetNormalized(),Position,R->GetScale3D());
    };
    // Avoid elbow/knee inversion and fully folded limbs from noisy landmarks.
    auto Bend=[](FVector Upper,FVector Lower)
    {
        const float Angle=FMath::Acos(FMath::Clamp(FVector::DotProduct(Upper,Lower),-1.,1.));
        if(Angle<FMath::DegreesToRadians(155.f))return Lower;
        return FQuat::Slerp(FQuat::Identity,FQuat::FindBetweenNormals(Upper,Lower),
            FMath::DegreesToRadians(155.f)/Angle).RotateVector(Upper).GetSafeNormal();
    };
    FVector Up=FVector::UpVector;
    if(Reliable(11,23)&&Reliable(12,24))
    {
        Up=((P.Joints[11]+P.Joints[12])-(P.Joints[23]+P.Joints[24])).GetSafeNormal();
        if(Up.Z<.25)Up=FVector(Up.X,Up.Y,.25).GetSafeNormal();
    }
    FVector Pelvis=Location(TEXT("pelvis"));
    if(Reliable(23,27)&&Reliable(24,28))
    {
        const float Feet=FMath::Min(P.Joints[27].Z,P.Joints[28].Z);
        const float Hip=(P.Joints[23].Z+P.Joints[24].Z)*.5f;
        Pelvis.Z=FMath::Clamp((Hip-Feet)*100/FMath::Max(P.Height,.5f)+4.f,18.f,58.f);
    }
    const FVector Spine=Pelvis+Up*Length(TEXT("pelvis"),TEXT("spine"));
    const FVector Chest=Spine+Up*Length(TEXT("spine"),TEXT("chest"));
    const FVector Neck=Chest+Up*Length(TEXT("chest"),TEXT("neck"));
    const FVector Head=Neck+Up*Length(TEXT("neck"),TEXT("head"));
    Set(TEXT("pelvis"),Pelvis,FVector::UpVector,Up);
    Set(TEXT("spine"),Spine,FVector::UpVector,Up);
    Set(TEXT("chest"),Chest,FVector::UpVector,Up);
    Set(TEXT("neck"),Neck,FVector::UpVector,Up);
    Set(TEXT("head"),Head,FVector::UpVector,Up);
    FVector Side=(Location(TEXT("upperarm_l"))-Location(TEXT("upperarm_r"))).GetSafeNormal();
    if(Reliable(12,11))Side=Direction(12,11,Side);
    Side=(Side-Up*FVector::DotProduct(Side,Up)).GetSafeNormal();
    if(Side.IsNearlyZero())Side=-FVector::RightVector;
    const float Speed=FMath::Clamp(P.Speed,0.f,3.f);
    const double Phase=Now*2*UE_PI*1.35;
    FVector Travel=P.LocalVelocity.GetSafeNormal();if(Travel.IsNearlyZero())Travel=FVector::ForwardVector;
    const float Swing=FMath::Min(Speed/1.2f,1.f);
    float Lowest=FLT_MAX;
    for(int32 S=0;S<2;++S)
    {
        const TCHAR* Suffix=S==0?TEXT("l"):TEXT("r");const float Sign=S==0?1.f:-1.f;
        auto Name=[&](const TCHAR* Part){return FName(*FString::Printf(TEXT("%s_%s"),Part,Suffix));};
        const FName Clav=Name(TEXT("clavicle")),Upper=Name(TEXT("upperarm")),Lower=Name(TEXT("lowerarm")),Hand=Name(TEXT("hand"));
        const FName Thigh=Name(TEXT("thigh")),Calf=Name(TEXT("calf")),Foot=Name(TEXT("foot"));
        const int Sh=S==0?11:12,El=S==0?13:14,Wr=S==0?15:16,Hi=S==0?23:24,Kn=S==0?25:26,An=S==0?27:28;
        const FVector ClavPos=Neck-Up*2.f;
        const FVector Shoulder=ClavPos+Side*Sign*Length(Clav,Upper);
        const float Wave=FMath::Sin(Phase+(S==0?0:UE_PI))*Swing;
        const FVector ArmFallback=(-Up+Side*Sign*.12f-Travel*Wave*.45f).GetSafeNormal();
        const FVector Arm=Direction(Sh,El,ArmFallback);
        const FVector Forearm=Bend(Arm,Direction(El,Wr,Arm+Travel*.12f));
        const FVector Elbow=Shoulder+Arm*Length(Upper,Lower);
        const FVector Wrist=Elbow+Forearm*Length(Lower,Hand);
        Set(Clav,ClavPos,Location(Upper)-Location(Clav),Side*Sign);
        Set(Upper,Shoulder,Location(Lower)-Location(Upper),Arm);
        Set(Lower,Elbow,Location(Hand)-Location(Lower),Forearm);
        Set(Hand,Wrist,Location(Hand)-Location(Lower),Forearm);
        const FVector Hip=Pelvis+Side*Sign*FMath::Abs((Location(Thigh)-Location(TEXT("pelvis"))).Size());
        const FVector Leg=Direction(Hi,Kn,(-Up+Travel*Wave*.35f).GetSafeNormal());
        const FVector Shin=Bend(Leg,Direction(Kn,An,(-Up-Travel*FMath::Max(0.f,Wave)*.45f).GetSafeNormal()));
        const FVector Knee=Hip+Leg*Length(Thigh,Calf);
        const FVector Ankle=Knee+Shin*Length(Calf,Foot);
        Set(Thigh,Hip,Location(Calf)-Location(Thigh),Leg);
        Set(Calf,Knee,Location(Foot)-Location(Calf),Shin);
        FVector FootDir=Direction(An,S==0?31:32,FVector::ForwardVector);
        if(!Reliable(An,S==0?31:32))FootDir=FVector::ForwardVector;
        // Feet stay approximately level; ankle/heel landmarks do not set scale.
        FootDir.Z=FMath::Clamp(FootDir.Z,-.35,.35);FootDir.Normalize();
        Set(Foot,Ankle,FVector::ForwardVector,FootDir);
        Lowest=FMath::Min(Lowest,float(Ankle.Z-Location(Foot).Z));
    }
    // Remove whole-body floor penetration without changing any bone length.
    if(FMath::IsFinite(Lowest))for(auto& Entry:Out)
        if(Entry.Key!=TEXT("root")&&Entry.Key!=TEXT("HumanSkeleton"))Entry.Value.AddToTranslation(FVector(0,0,-Lowest));
    return Out;
}
