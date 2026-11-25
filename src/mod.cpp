#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.hpp"
#include "mods/svc/ui.h"

#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "d/d_camera.h"
#include "d/d_meter2_draw.h"
#include "m_Do/m_Do_controller_pad.h"

namespace dusk {
enum class ActionBinds : int;
}

DEFINE_MOD();

IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService, svc_ui);

DEFINE_HOOK(&dCamera_c::updatePad, CameraUpdatePad);
DEFINE_HOOK(&dCamera_c::nextMode, CameraNextMode);
DEFINE_HOOK(&daAlink_c::setBodyAngleToCamera, LinkCameraLook);
DEFINE_HOOK(&daAlink_c::setSpeedAndAngleNormal, LinkSpeedAngleNormal);
DEFINE_HOOK(&daAlink_c::procMoveTurn, LinkMoveTurn);
DEFINE_HOOK(&daAlink_c::execute, LinkExecute);
DEFINE_HOOK(&daAlink_c::setBgCheckParam, LinkSetBgCheckParam);
DEFINE_HOOK(&daAlink_c::procCoSubjectivity, LinkSubjectivity);
DEFINE_HOOK(&dMeter2Draw_c::getCameraSubject, MeterCameraSubject);
DEFINE_HOOK_SYMBOL("dusk::getActionBindTrig", bool(dusk::ActionBinds, u32), ActionBindingTrigger);

namespace {

constexpr s32 kMaxFirstPersonPitch = 0x3F00;  // Avoid the vertical camera-tracking singularity.
constexpr f32 kFirstPersonCollisionRadiusScale = 0.75f;
constexpr f32 kFirstPersonTopOutCollisionRadiusScale = 0.65f;
constexpr f32 kFirstPersonActorCollisionPushScale = 1.5f;
constexpr f32 kFirstPersonWallRecoveryDistance = 3.0f;
constexpr f32 kNormalSwordAttackRadius = 20.0f;
constexpr f32 kFirstPersonSwordAttackRadius = 25.0f;
constexpr int kLinkCollisionCircleCount = 3;
constexpr u8 kWallStuckFramesBeforeRecovery = 4;
// OPEN_MAP_SCREEN is the third ActionBinds enumerator in Dusklight.
constexpr dusk::ActionBinds kOpenMapScreenAction =
    static_cast<dusk::ActionBinds>(2);

struct StickState {
    f32 x;
    f32 y;
    f32 value;
    s16 angle;
};

struct CameraInputState {
    interface_of_controller_pad* pad = nullptr;
    StickState subStick{};
};

CameraInputState s_cameraInput;
dCamera_c* s_camera = nullptr;
bool s_cameraExitRequested = false;
bool s_cameraLookUpdatedDuringExecute = false;
s16 s_cameraPitchBeforeExecute = 0;
s16 s_cameraPitchAfterLook = 0;
bool s_cameraPitchSavedForExecute = false;
bool s_linkWasOnLadderDuringExecute = false;
bool s_wasOnLadderLastExecute = false;
bool s_linkWasSwordPushingDuringExecute = false;
bool s_wasSwordPushingLastExecute = false;
ConfigVarHandle s_autoReactivateConfig = 0;
bool s_autoReactivateAfterAreaTransitions = false;
bool s_wasFirstPerson = false;
bool s_areaTransitionObserved = false;
bool s_autoReactivationPending = false;
bool s_forcedCameraMode = false;
u8 s_forcedCameraModeFrames = 0;
daAlink_c* s_collisionRadiusLink = nullptr;
f32 s_originalCollisionRadii[kLinkCollisionCircleCount]{};
bool s_collisionRadiusReduced = false;
f32 s_appliedCollisionRadiusScale = 1.0f;
cXyz s_linkPositionBeforeExecute{};
cXyz s_actorCollisionMoveBeforeExecute{};
f32 s_moveValueBeforeExecute = 0.0f;
u8 s_wallStuckFrames = 0;
bool s_firstPersonCollisionRecoveryEnabled = false;

StickState read_sub_stick(const interface_of_controller_pad& pad) {
    return {pad.mCStickPosX, pad.mCStickPosY, pad.mCStickValue, pad.mCStickAngle};
}

void write_sub_stick(interface_of_controller_pad& pad, StickState stick) {
    pad.mCStickPosX = stick.x;
    pad.mCStickPosY = stick.y;
    pad.mCStickValue = stick.value;
    pad.mCStickAngle = stick.angle;
}

bool is_ladder_or_climb_process(int process) {
    switch (process) {
    case daAlink_c::PROC_LADDER_UP_START:
    case daAlink_c::PROC_LADDER_UP_END:
    case daAlink_c::PROC_LADDER_DOWN_START:
    case daAlink_c::PROC_LADDER_DOWN_END:
    case daAlink_c::PROC_LADDER_MOVE:
    case daAlink_c::PROC_CLIMB_UP_START:
    case daAlink_c::PROC_CLIMB_DOWN_START:
    case daAlink_c::PROC_CLIMB_MOVE_UPDOWN:
    case daAlink_c::PROC_CLIMB_MOVE_SIDE:
    case daAlink_c::PROC_CLIMB_WAIT:
    case daAlink_c::PROC_CLIMB_TO_ROOF:
        return true;
    default:
        return false;
    }
}

bool is_ladder_top_out_process(int process) {
    return process == daAlink_c::PROC_LADDER_UP_END ||
           process == daAlink_c::PROC_CLIMB_TO_ROOF;
}

bool is_jump_or_landing_process(int process) {
    switch (process) {
    case daAlink_c::PROC_BACK_JUMP:
    case daAlink_c::PROC_BACK_JUMP_LAND:
    case daAlink_c::PROC_AUTO_JUMP:
    case daAlink_c::PROC_DIVE_JUMP:
    case daAlink_c::PROC_ROLL_JUMP:
    case daAlink_c::PROC_FALL:
    case daAlink_c::PROC_LAND:
    case daAlink_c::PROC_SMALL_JUMP:
    case daAlink_c::PROC_CUT_FINISH_JUMP_UP:
    case daAlink_c::PROC_CUT_FINISH_JUMP_UP_LAND:
    case daAlink_c::PROC_CUT_JUMP:
    case daAlink_c::PROC_CUT_JUMP_LAND:
    case daAlink_c::PROC_CUT_LARGE_JUMP_CHARGE:
    case daAlink_c::PROC_CUT_LARGE_JUMP:
    case daAlink_c::PROC_CUT_LARGE_JUMP_LAND:
    case daAlink_c::PROC_LAND_DAMAGE:
        return true;
    default:
        return false;
    }
}

f32 get_first_person_collision_radius_scale(int process) {
    return is_ladder_top_out_process(process)
               ? kFirstPersonTopOutCollisionRadiusScale
               : kFirstPersonCollisionRadiusScale;
}

void restore_first_person_collision_radius(daAlink_c* link) {
    if (!s_collisionRadiusReduced || s_collisionRadiusLink != link) {
        return;
    }

    for (int i = 0; i < kLinkCollisionCircleCount; ++i) {
        link->mAcchCir[i].SetWallR(s_originalCollisionRadii[i]);
    }
    s_collisionRadiusLink = nullptr;
    s_collisionRadiusReduced = false;
    s_appliedCollisionRadiusScale = 1.0f;
}

void reduce_first_person_collision_radius(
    daAlink_c* link, bool refreshOriginal, f32 scale)
{
    if (s_collisionRadiusReduced && s_collisionRadiusLink == link && !refreshOriginal) {
        if (s_appliedCollisionRadiusScale != scale) {
            for (int i = 0; i < kLinkCollisionCircleCount; ++i) {
                link->mAcchCir[i].SetWallR(s_originalCollisionRadii[i] * scale);
            }
            s_appliedCollisionRadiusScale = scale;
        }
        return;
    }

    if (s_collisionRadiusReduced && s_collisionRadiusLink != link) {
        restore_first_person_collision_radius(s_collisionRadiusLink);
    }

    s_collisionRadiusLink = link;
    for (int i = 0; i < kLinkCollisionCircleCount; ++i) {
        s_originalCollisionRadii[i] = link->mAcchCir[i].GetWallR();
        link->mAcchCir[i].SetWallR(s_originalCollisionRadii[i] * scale);
    }
    s_collisionRadiusReduced = true;
    s_appliedCollisionRadiusScale = scale;
}

void prepare_link_collision_for_first_person() {
    if (auto* link = static_cast<daAlink_c*>(daPy_getPlayerActorClass())) {
        if (!is_jump_or_landing_process(link->mProcID)) {
            reduce_first_person_collision_radius(
                link, false, get_first_person_collision_radius_scale(link->mProcID));
        }
    }
}

void recover_from_first_person_wall_stuck(daAlink_c* link) {
    if (!s_firstPersonCollisionRecoveryEnabled) {
        s_wallStuckFrames = 0;
        return;
    }

    const bool wallContact = link->mLinkAcch.ChkWallHit();
    const f32 actorPushLength = s_actorCollisionMoveBeforeExecute.absXZ();
    const bool actorContact = actorPushLength > 0.1f;
    if (!wallContact && !actorContact) {
        s_wallStuckFrames = 0;
        return;
    }
    if (wallContact && !actorContact && s_moveValueBeforeExecute <= 0.05f) {
        s_wallStuckFrames = 0;
        return;
    }

    const f32 movedX = link->current.pos.x - s_linkPositionBeforeExecute.x;
    const f32 movedZ = link->current.pos.z - s_linkPositionBeforeExecute.z;
    if (movedX * movedX + movedZ * movedZ >= 1.0f) {
        s_wallStuckFrames = 0;
        return;
    }

    if (++s_wallStuckFrames < kWallStuckFramesBeforeRecovery) {
        return;
    }

    if (wallContact) {
        for (int i = 0; i < kLinkCollisionCircleCount; ++i) {
            if (link->mAcchCir[i].ChkWallHit()) {
                const s16 wallAngle = link->mAcchCir[i].GetWallAngleY();
                link->current.pos.x +=
                    kFirstPersonWallRecoveryDistance * cM_ssin(wallAngle);
                link->current.pos.z +=
                    kFirstPersonWallRecoveryDistance * cM_scos(wallAngle);
                s_wallStuckFrames = 0;
                return;
            }
        }
    }

    if (actorContact) {
        link->current.pos.x += kFirstPersonWallRecoveryDistance *
                               s_actorCollisionMoveBeforeExecute.x / actorPushLength;
        link->current.pos.z += kFirstPersonWallRecoveryDistance *
                               s_actorCollisionMoveBeforeExecute.z / actorPushLength;
    }
    s_wallStuckFrames = 0;
}

HookAction on_camera_update_pad_pre(ModContext*, void* args, void*, void*) {
    auto* camera = mods::arg<dCamera_c*>(args, 0);
    s_camera = camera;
    s_cameraInput.pad = nullptr;
    if (camera->Mode() == 4 && !s_areaTransitionObserved) {
        s_wasFirstPerson = true;
    }
    if (camera->Mode() != 4 || camera->mPadID >= 4) {
        return HOOK_CONTINUE;
    }

    auto& pad = mDoCPd_c::getCpadInfo(camera->mPadID);
    s_cameraInput.pad = &pad;
    s_cameraInput.subStick = read_sub_stick(pad);
    write_sub_stick(pad, {});
    return HOOK_CONTINUE;
}

HookAction on_camera_next_mode_pre(ModContext*, void* args, void* retval, void*) {
    auto* camera = mods::arg<dCamera_c*>(args, 0);
    if (mods::arg<s32>(args, 1) != 4) {
        return HOOK_CONTINUE;
    }

    if (ActionBindingTrigger::g_orig(
            static_cast<dusk::ActionBinds>(0), camera->mPadID))
    {
        s_wasFirstPerson = false;
        s_autoReactivationPending = false;
        s_cameraExitRequested = true;
        dComIfGp_clearPlayerStatus0(0, 0x2000);
        return HOOK_CONTINUE;
    }

    if (s_cameraExitRequested) {
        return HOOK_CONTINUE;
    }

    prepare_link_collision_for_first_person();
    dComIfGp_setPlayerStatus0(0, 0x2000);
    *static_cast<s32*>(retval) = 4;
    return HOOK_SKIP_ORIGINAL;
}

void on_camera_next_mode_post(ModContext*, void* args, void* retval, void*) {
    if (mods::arg<s32>(args, 1) == 4 && *static_cast<s32*>(retval) != 4) {
        s_cameraExitRequested = false;
    }
}

void on_camera_update_pad_post(ModContext*, void*, void*, void*) {
    if (s_cameraInput.pad == nullptr) {
        return;
    }

    write_sub_stick(*s_cameraInput.pad, s_cameraInput.subStick);
    s_cameraInput.pad = nullptr;
}

HookAction on_action_binding_trigger_pre(ModContext*, void* args, void* retval, void*) {
    const auto action = mods::arg<dusk::ActionBinds>(args, 0);
    const u32 port = mods::arg<u32>(args, 1);
    if (action == kOpenMapScreenAction && port < 4 &&
        s_camera != nullptr && port == s_camera->mPadID && s_camera->Mode() == 4 &&
        mDoCPd_c::getTrigRight(port))
    {
        *static_cast<bool*>(retval) = true;
        return HOOK_SKIP_ORIGINAL;
    }

    return HOOK_CONTINUE;
}

HookAction on_meter_camera_subject_pre(ModContext*, void*, void* retval, void*) {
    if (s_camera != nullptr && s_camera->Mode() == 4) {
        *static_cast<bool*>(retval) = false;
        return HOOK_SKIP_ORIGINAL;
    }

    return HOOK_CONTINUE;
}

HookAction on_link_subjectivity_pre(ModContext*, void* args, void* retval, void*) {
    auto* link = mods::arg<daAlink_c*>(args, 0);
    if (link->mProcID != daAlink_c::PROC_SUBJECTIVITY || s_camera == nullptr ||
        s_camera->Mode() != 4)
    {
        return HOOK_CONTINUE;
    }

    *static_cast<int*>(retval) = link->procWaitInit();
    return HOOK_SKIP_ORIGINAL;
}

struct LinkLookInput {
    daAlink_c* link = nullptr;
    f32 moveValue = 0.0f;
    s16 stickAngle = 0;
    f32 subStickValue = 0.0f;
    s16 subStickAngle = 0;
    s16 bodyAngleX = 0;
};

LinkLookInput s_linkLookInput;
int s_pitchAxisSign = 1;  // Learn the effective sign to respect Dusklight's Y-axis inversion setting.

HookAction on_link_camera_look_pre(ModContext*, void* args, void*, void*) {
    auto* link = mods::arg<daAlink_c*>(args, 0);
    s_linkLookInput.link = nullptr;
    if (s_camera == nullptr || s_camera->Mode() != 4) {
        return HOOK_CONTINUE;
    }

    s_linkLookInput.link = link;
    s_linkLookInput.moveValue = link->mMoveValue;
    s_linkLookInput.stickAngle = link->mStickAngle;
    s_linkLookInput.bodyAngleX = link->mBodyAngle.x;
    s_linkLookInput.subStickValue = mDoCPd_c::getSubStickValue(PAD_1);
    s_linkLookInput.subStickAngle = static_cast<s16>(
        static_cast<u16>(mDoCPd_c::getSubStickAngle(PAD_1)) + 0x8000u);
    if (!is_ladder_or_climb_process(link->mProcID)) {
        link->shape_angle.y = link->getCameraAngleY();
    }
    link->mMoveValue = s_linkLookInput.subStickValue;
    link->mStickAngle = s_linkLookInput.subStickAngle;
    return HOOK_CONTINUE;
}

void on_link_camera_look_post(ModContext*, void*, void* retval, void*) {
    if (s_linkLookInput.link == nullptr) {
        return;
    }

    auto* link = s_linkLookInput.link;
    const bool cameraUpdated = *static_cast<BOOL*>(retval) != 0;
    s_cameraLookUpdatedDuringExecute = cameraUpdated;

    if (cameraUpdated) {
        const f32 pitchComponent = cM_scos(s_linkLookInput.subStickAngle);
        const f32 pitchMagnitude = pitchComponent < 0.0f ? -pitchComponent : pitchComponent;
        const s16 pitchDelta = static_cast<s16>(
            link->mBodyAngle.x - s_linkLookInput.bodyAngleX);
        const int stickDirection = pitchComponent < 0.0f ? -1 : 1;
        const bool withinNativePitchRange =
            link->mpHIO == nullptr ||
            (s_linkLookInput.bodyAngleX <= link->mpHIO->mItem.m.mItemFPMaxUnk &&
             s_linkLookInput.bodyAngleX >= link->mpHIO->mItem.m.mItemFPUpMaxUnk);

        if (pitchDelta != 0 && s_linkLookInput.subStickValue > 0.05f &&
            pitchMagnitude > 0.05f &&
            withinNativePitchRange && !link->checkNotItemSinkLimit())
        {
            s_pitchAxisSign = (pitchDelta < 0 ? -1 : 1) * stickDirection;
        }

        // Reapply pitch from raw input to bypass Dusklight's configured item-view limit.
        const f32 pitchStep = s_linkLookInput.subStickValue > 0.05f
                                  ? 512.0f * s_linkLookInput.subStickValue *
                                        s_linkLookInput.subStickValue * pitchMagnitude
                                  : 0.0f;
        const int pitchDirection = stickDirection * s_pitchAxisSign;
        s32 pitch = static_cast<s32>(
            s_linkLookInput.bodyAngleX + pitchDirection * pitchStep);
        if (pitch > kMaxFirstPersonPitch) {
            pitch = kMaxFirstPersonPitch;
        } else if (pitch < -kMaxFirstPersonPitch) {
            pitch = -kMaxFirstPersonPitch;
        }
        const s16 limitedPitch = static_cast<s16>(pitch);

        if (link->mBodyAngle.x != limitedPitch) {
            link->mBodyAngle.x = limitedPitch;
            link->checkBodyAngleX(link->mBodyAngle.x);
        }
        link->field_0x310a = link->mBodyAngle.x;
        s_cameraPitchAfterLook = link->mBodyAngle.x;
    }

    link->mMoveValue = s_linkLookInput.moveValue;
    link->mStickAngle = s_linkLookInput.stickAngle;
    s_linkLookInput.link = nullptr;
}

void on_link_speed_angle_normal_post(ModContext*, void* args, void*, void*) {
    auto* link = mods::arg<daAlink_c*>(args, 0);
    if (s_camera != nullptr && s_camera->Mode() == 4 &&
        !is_ladder_or_climb_process(link->mProcID))
    {
        link->shape_angle.y = link->getCameraAngleY();
    }
}

HookAction on_link_speed_angle_normal_pre(ModContext*, void* args, void*, void*) {
    auto* link = mods::arg<daAlink_c*>(args, 0);
    if (s_camera != nullptr && s_camera->Mode() == 4 &&
        !link->checkMagneBootsOn() && !is_ladder_or_climb_process(link->mProcID))
    {
        link->mMoveAngle = static_cast<s16>(
            link->mStickAngle + link->getCameraAngleY());
    }

    return HOOK_CONTINUE;
}

void on_link_move_turn_post(ModContext*, void* args, void*, void*) {
    auto* link = mods::arg<daAlink_c*>(args, 0);
    if (s_camera != nullptr && s_camera->Mode() == 4 &&
        !is_ladder_or_climb_process(link->mProcID))
    {
        link->shape_angle.y = link->getCameraAngleY();
    }
}

HookAction on_link_execute_pre(ModContext*, void* args, void*, void*) {
    auto* link = mods::arg<daAlink_c*>(args, 0);
    s_linkWasOnLadderDuringExecute = is_ladder_or_climb_process(link->mProcID);
    s_linkWasSwordPushingDuringExecute = link->mProcID == daAlink_c::PROC_SWORD_PUSH;
    const bool areaTransitionStarted = link->checkSceneChangeAreaStart();
    if (areaTransitionStarted && !s_areaTransitionObserved) {
        s_autoReactivationPending =
            s_autoReactivateAfterAreaTransitions &&
            (s_wasFirstPerson || (s_camera != nullptr && s_camera->Mode() == 4));
        s_wasFirstPerson = false;
    }
    s_areaTransitionObserved = areaTransitionStarted;

    if (!s_autoReactivateAfterAreaTransitions) {
        s_autoReactivationPending = false;
        if (s_forcedCameraMode && s_camera != nullptr) {
            s_camera->ModeFix(11);
            s_forcedCameraMode = false;
            s_forcedCameraModeFrames = 0;
        }
    } else if (s_camera != nullptr) {
        if (s_forcedCameraMode) {
            if (s_camera->Mode() == 4 || ++s_forcedCameraModeFrames >= 4) {
                s_camera->ModeFix(11);
                s_forcedCameraMode = false;
                s_forcedCameraModeFrames = 0;
            }
        }

        const bool playerCanMove =
            (link->mProcID == daAlink_c::PROC_WAIT || link->mProcID == daAlink_c::PROC_MOVE) &&
            !areaTransitionStarted && !dComIfGp_event_runCheck();
        if (s_autoReactivationPending && playerCanMove) {
            if (s_camera->Mode() != 4) {
                prepare_link_collision_for_first_person();
                dComIfGp_setPlayerStatus0(0, 0x2000);
                s_camera->ModeFix(4);
                s_forcedCameraMode = true;
                s_forcedCameraModeFrames = 0;
            }
            s_autoReactivationPending = false;
        }
    }

    if (s_camera != nullptr && s_camera->Mode() == 4 &&
        !is_jump_or_landing_process(link->mProcID))
    {
        reduce_first_person_collision_radius(
            link, false, get_first_person_collision_radius_scale(link->mProcID));
    } else {
        restore_first_person_collision_radius(link);
    }

    s_firstPersonCollisionRecoveryEnabled =
        s_camera != nullptr && s_camera->Mode() == 4 &&
        !is_ladder_or_climb_process(link->mProcID) &&
        !is_jump_or_landing_process(link->mProcID);
    s_linkPositionBeforeExecute = link->current.pos;
    s_moveValueBeforeExecute = link->mMoveValue;
    const f32 swordRadius =
        s_camera != nullptr && s_camera->Mode() == 4
            ? kFirstPersonSwordAttackRadius
            : kNormalSwordAttackRadius;
    for (int i = 0; i < 3; ++i) {
        link->mAtCps[i].SetR(swordRadius);
    }

    if (s_firstPersonCollisionRecoveryEnabled) {
        cXyz* collisionMove = link->mCcStts.GetCCMoveP();
        s_actorCollisionMoveBeforeExecute = *collisionMove;
        collisionMove->x *= kFirstPersonActorCollisionPushScale;
        collisionMove->z *= kFirstPersonActorCollisionPushScale;
    } else {
        s_actorCollisionMoveBeforeExecute = cXyz::Zero;
        s_wallStuckFrames = 0;
    }

    s_cameraLookUpdatedDuringExecute = false;
    s_cameraPitchSavedForExecute =
        s_camera != nullptr && s_camera->Mode() == 4;
    if (s_cameraPitchSavedForExecute) {
        s_cameraPitchBeforeExecute = link->mBodyAngle.x;
    }
    return HOOK_CONTINUE;
}

void on_link_set_bg_check_param_post(ModContext*, void* args, void*, void*) {
    auto* link = mods::arg<daAlink_c*>(args, 0);
    if (s_camera != nullptr && s_camera->Mode() == 4 &&
        !is_jump_or_landing_process(link->mProcID))
    {
        reduce_first_person_collision_radius(
            link, true, get_first_person_collision_radius_scale(link->mProcID));
    } else {
        s_collisionRadiusLink = nullptr;
        s_collisionRadiusReduced = false;
        s_appliedCollisionRadiusScale = 1.0f;
    }
}

void on_link_execute_post(ModContext*, void* args, void*, void*) {
    auto* link = mods::arg<daAlink_c*>(args, 0);
    const bool isOnLadder = is_ladder_or_climb_process(link->mProcID);
    const bool isSwordPushing = link->mProcID == daAlink_c::PROC_SWORD_PUSH;
    const bool linkWasOnLadder = s_linkWasOnLadderDuringExecute || s_wasOnLadderLastExecute ||
                                 isOnLadder;
    const bool justLeftLadder =
        (s_linkWasOnLadderDuringExecute || s_wasOnLadderLastExecute) && !isOnLadder;
    const bool justLeftSwordPush =
        (s_linkWasSwordPushingDuringExecute || s_wasSwordPushingLastExecute) &&
        !isSwordPushing;
    s_linkWasOnLadderDuringExecute = false;
    s_wasOnLadderLastExecute = isOnLadder;
    s_linkWasSwordPushingDuringExecute = false;
    s_wasSwordPushingLastExecute = isSwordPushing;
    if (s_camera == nullptr || s_camera->Mode() != 4) {
        s_firstPersonCollisionRecoveryEnabled = false;
        s_wallStuckFrames = 0;
        s_cameraPitchSavedForExecute = false;
        return;
    }

    recover_from_first_person_wall_stuck(link);

    dComIfGp_setPlayerStatus0(0, 0x2000);

    if (s_cameraPitchSavedForExecute && s_cameraLookUpdatedDuringExecute) {
        link->mBodyAngle.x = s_cameraPitchAfterLook;
        link->field_0x310a = s_cameraPitchAfterLook;
    } else {
        if (s_cameraPitchSavedForExecute) {
            link->mBodyAngle.x = s_cameraPitchBeforeExecute;
            link->field_0x310a = s_cameraPitchBeforeExecute;
        }
        link->setBodyAngleToCamera();
    }

    if (linkWasOnLadder) {
        link->field_0x310c = link->shape_angle.y;
        s_camera->U2(link->shape_angle.y);
        if (justLeftLadder) {
            dComIfGp_setPlayerStatus0(0, 0x2000);
            link->current.angle.y = link->shape_angle.y;
            if (!link->checkMagneBootsOn()) {
                link->mMoveAngle = static_cast<s16>(
                    link->mStickAngle + link->getCameraAngleY());
            }
        }
    } else {
        const s16 cameraYaw = link->getCameraAngleY();
        link->shape_angle.y = cameraYaw;
        if (justLeftSwordPush) {
            link->field_0x310c = cameraYaw;
            link->current.angle.y = cameraYaw;
            if (!link->checkMagneBootsOn()) {
                link->mMoveAngle = static_cast<s16>(link->mStickAngle + cameraYaw);
            }
        }
    }
    s_cameraPitchSavedForExecute = false;
}

void on_auto_reactivation_setting_changed(ModContext*, ConfigVarHandle,
    const ConfigVarValue* value, const ConfigVarValue*, void*) {
    if (value == nullptr || value->type != CONFIG_VAR_BOOL) {
        return;
    }
    s_autoReactivateAfterAreaTransitions = value->bool_value;
    if (!s_autoReactivateAfterAreaTransitions) {
        s_autoReactivationPending = false;
        if (s_forcedCameraMode && s_camera != nullptr) {
            s_camera->ModeFix(11);
            s_forcedCameraMode = false;
            s_forcedCameraModeFrames = 0;
        }
    }
}

ModResult build_mods_panel(ModContext* ctx, UiElementHandle panel, void*, ModError*) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = "Re-enter First Person after Area Transitions";
    control.help_rml =
        "Automatically restores first-person view after an area transition, once Link can move "
        "again.";
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = s_autoReactivateConfig;
    return svc_ui->pane_add_control(ctx, panel, &control, nullptr);
}

ModResult register_area_transition_option() {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = "autoReactivateAfterAreaTransitions";
    desc.type = CONFIG_VAR_BOOL;
    desc.default_bool = false;
    ModResult result = svc_config->register_var(mod_ctx, &desc, &s_autoReactivateConfig);
    if (result != MOD_OK) {
        mods::log::error("failed to register area-transition first-person option");
        return result;
    }

    result = svc_config->get_bool(
        mod_ctx, s_autoReactivateConfig, &s_autoReactivateAfterAreaTransitions);
    if (result != MOD_OK) {
        mods::log::error("failed to read area-transition first-person option");
        return result;
    }

    result = svc_config->subscribe(
        mod_ctx, s_autoReactivateConfig, on_auto_reactivation_setting_changed, nullptr, nullptr);
    if (result != MOD_OK) {
        mods::log::error("failed to subscribe to area-transition first-person option");
    }
    return result;
}

}  // namespace

extern "C" {
MOD_EXPORT ModResult mod_initialize(ModError*) {
    ModResult result = register_area_transition_option();
    if (result != MOD_OK) {
        return result;
    }

    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = build_mods_panel;
    result = svc_ui->register_mods_panel(mod_ctx, &panel);
    if (result != MOD_OK) {
        mods::log::error("failed to register first-person mod settings panel");
        return result;
    }

    result = mods::hook::add_pre<LinkSubjectivity>(on_link_subjectivity_pre);
    if (result != MOD_OK) {
        mods::log::error("failed to install first-person movement pre-hook");
        return result;
    }

    result = mods::hook::add_pre<LinkExecute>(on_link_execute_pre);
    if (result != MOD_OK) {
        mods::hook::uninstall<LinkSubjectivity>();
        mods::log::error("failed to install first-person camera update pre-hook");
        return result;
    }

    result = mods::hook::add_post<LinkExecute>(on_link_execute_post);
    if (result != MOD_OK) {
        mods::hook::uninstall<LinkExecute>();
        mods::hook::uninstall<LinkSubjectivity>();
        mods::log::error("failed to install first-person camera update post-hook");
        return result;
    }

    result = mods::hook::add_pre<LinkCameraLook>(on_link_camera_look_pre);
    if (result != MOD_OK) {
        mods::hook::uninstall<LinkExecute>();
        mods::hook::uninstall<LinkSubjectivity>();
        mods::log::error("failed to install first-person camera look pre-hook");
        return result;
    }

    result = mods::hook::add_post<LinkCameraLook>(on_link_camera_look_post);
    if (result != MOD_OK) {
        mods::hook::uninstall<LinkCameraLook>();
        mods::hook::uninstall<LinkExecute>();
        mods::hook::uninstall<LinkSubjectivity>();
        mods::log::error("failed to install first-person camera look post-hook");
        return result;
    }

    result = mods::hook::add_pre<CameraUpdatePad>(on_camera_update_pad_pre);
    if (result != MOD_OK) {
        mods::hook::uninstall<LinkCameraLook>();
        mods::hook::uninstall<LinkExecute>();
        mods::hook::uninstall<LinkSubjectivity>();
        mods::log::error("failed to install first-person camera exit-gesture pre-hook");
        return result;
    }

    result = mods::hook::add_post<CameraUpdatePad>(on_camera_update_pad_post);
    if (result != MOD_OK) {
        mods::hook::uninstall<CameraUpdatePad>();
        mods::hook::uninstall<LinkCameraLook>();
        mods::hook::uninstall<LinkExecute>();
        mods::hook::uninstall<LinkSubjectivity>();
        mods::log::error("failed to install first-person camera exit-gesture post-hook");
        return result;
    }

    result = mods::hook::install<ActionBindingTrigger>();
    if (result != MOD_OK) {
        mods::hook::uninstall<CameraUpdatePad>();
        mods::hook::uninstall<LinkCameraLook>();
        mods::hook::uninstall<LinkExecute>();
        mods::hook::uninstall<LinkSubjectivity>();
        mods::log::error("failed to install first-person binding resolver");
        return result;
    }

    result = mods::hook::add_pre<ActionBindingTrigger>(on_action_binding_trigger_pre);
    if (result != MOD_OK) {
        mods::hook::uninstall<ActionBindingTrigger>();
        mods::hook::uninstall<CameraUpdatePad>();
        mods::hook::uninstall<LinkCameraLook>();
        mods::hook::uninstall<LinkExecute>();
        mods::hook::uninstall<LinkSubjectivity>();
        mods::log::error("failed to install first-person map button hook");
        return result;
    }

    result = mods::hook::add_pre<CameraNextMode>(on_camera_next_mode_pre);
    if (result != MOD_OK) {
        mods::hook::uninstall<ActionBindingTrigger>();
        mods::hook::uninstall<CameraUpdatePad>();
        mods::hook::uninstall<LinkCameraLook>();
        mods::hook::uninstall<LinkExecute>();
        mods::hook::uninstall<LinkSubjectivity>();
        mods::log::error("failed to install first-person camera mode pre-hook");
        return result;
    }

    result = mods::hook::add_post<CameraNextMode>(on_camera_next_mode_post);
    if (result != MOD_OK) {
        mods::hook::uninstall<CameraNextMode>();
        mods::hook::uninstall<ActionBindingTrigger>();
        mods::hook::uninstall<CameraUpdatePad>();
        mods::hook::uninstall<LinkCameraLook>();
        mods::hook::uninstall<LinkExecute>();
        mods::hook::uninstall<LinkSubjectivity>();
        mods::log::error("failed to install first-person camera mode post-hook");
        return result;
    }

    result = mods::hook::add_pre<LinkSpeedAngleNormal>(on_link_speed_angle_normal_pre);
    if (result != MOD_OK) {
        mods::hook::uninstall<LinkSpeedAngleNormal>();
        mods::hook::uninstall<CameraNextMode>();
        mods::hook::uninstall<ActionBindingTrigger>();
        mods::hook::uninstall<CameraUpdatePad>();
        mods::hook::uninstall<LinkCameraLook>();
        mods::hook::uninstall<LinkExecute>();
        mods::hook::uninstall<LinkSubjectivity>();
        mods::log::error("failed to install first-person movement direction pre-hook");
        return result;
    }

    result = mods::hook::add_post<LinkSpeedAngleNormal>(on_link_speed_angle_normal_post);
    if (result != MOD_OK) {
        mods::hook::uninstall<LinkSpeedAngleNormal>();
        mods::hook::uninstall<CameraNextMode>();
        mods::hook::uninstall<ActionBindingTrigger>();
        mods::hook::uninstall<CameraUpdatePad>();
        mods::hook::uninstall<LinkCameraLook>();
        mods::hook::uninstall<LinkExecute>();
        mods::hook::uninstall<LinkSubjectivity>();
        mods::log::error("failed to install first-person movement facing post-hook");
        return result;
    }

    result = mods::hook::add_post<LinkMoveTurn>(on_link_move_turn_post);
    if (result != MOD_OK) {
        mods::hook::uninstall<LinkMoveTurn>();
        mods::hook::uninstall<LinkSpeedAngleNormal>();
        mods::hook::uninstall<CameraNextMode>();
        mods::hook::uninstall<ActionBindingTrigger>();
        mods::hook::uninstall<CameraUpdatePad>();
        mods::hook::uninstall<LinkCameraLook>();
        mods::hook::uninstall<LinkExecute>();
        mods::hook::uninstall<LinkSubjectivity>();
        mods::log::error("failed to install first-person skid facing post-hook");
        return result;
    }

    result = mods::hook::add_pre<MeterCameraSubject>(on_meter_camera_subject_pre);
    if (result != MOD_OK) {
        mods::hook::uninstall<LinkMoveTurn>();
        mods::hook::uninstall<LinkSpeedAngleNormal>();
        mods::hook::uninstall<CameraNextMode>();
        mods::hook::uninstall<ActionBindingTrigger>();
        mods::hook::uninstall<CameraUpdatePad>();
        mods::hook::uninstall<LinkCameraLook>();
        mods::hook::uninstall<LinkExecute>();
        mods::hook::uninstall<LinkSubjectivity>();
        mods::log::error("failed to install first-person HUD visibility hook");
        return result;
    }

    result = mods::hook::add_post<LinkSetBgCheckParam>(on_link_set_bg_check_param_post);
    if (result != MOD_OK) {
        mods::hook::uninstall<LinkSetBgCheckParam>();
        mods::hook::uninstall<MeterCameraSubject>();
        mods::hook::uninstall<LinkMoveTurn>();
        mods::hook::uninstall<LinkSpeedAngleNormal>();
        mods::hook::uninstall<CameraNextMode>();
        mods::hook::uninstall<ActionBindingTrigger>();
        mods::hook::uninstall<CameraUpdatePad>();
        mods::hook::uninstall<LinkCameraLook>();
        mods::hook::uninstall<LinkExecute>();
        mods::hook::uninstall<LinkSubjectivity>();
        mods::log::error("failed to install first-person collision radius hook");
        return result;
    }

    mods::log::info("First Person Controls initialized");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    return MOD_OK;
}
}
