#pragma once

#include "Base.h"

struct AnimationData
{
    Red::CName controller;
    bool debug = false;
    Locomotion_Style style{LS_Idle};
    MotionTableAction action{MTA_None};
    float path = 0.f;
    float slope = 0.f;
    float delta = 0.f;
    bool stairs = false;
    float ground = 0.f;
    float time = 0.f;
    float footScale = 1.f;
    float startAngle = 0.f;
    // AnimFeature_Locomotion.areAnimWrappersUnlocked, by its place (the feature's other bool is isOnStairs). Locked,
    // the locomotion ignores anim wrappers, and the stances the others' characters are shown with are wrappers:
    // crouching (inCrouch), a drawn weapon held ready (combatLocomotion). They never showed.
    bool wrappersUnlocked = true;
    float speed = 0.f;
};
