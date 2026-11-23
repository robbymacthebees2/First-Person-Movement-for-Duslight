#include "mods/service.hpp"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.hpp"

#include "d/actor/d_a_alink.h"
#include "d/d_camera.h"
#include "m_Do/m_Do_controller_pad.h"

DEFINE_MOD();

IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);

DEFINE_HOOK(&dCamera_c::updatePad, CameraUpdatePad);
DEFINE_HOOK(&daAlink_c::setBodyAngleToCamera, LinkCameraLook);
DEFINE_HOOK(&daAlink_c::procCoSubjectivity, LinkSubjectivity);

namespace {

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

StickState read_sub_stick(const interface_of_controller_pad& pad) {
    return {pad.mCStickPosX, pad.mCStickPosY, pad.mCStickValue, pad.mCStickAngle};
}

void write_sub_stick(interface_of_controller_pad& pad, StickState stick) {
    pad.mCStickPosX = stick.x;
    pad.mCStickPosY = stick.y;
    pad.mCStickValue = stick.value;
    pad.mCStickAngle = stick.angle;
}

HookAction on_camera_update_pad_pre(ModContext*, void* args, void*, void*) {
    auto* camera = mods::arg<dCamera_c*>(args, 0);
    s_cameraInput.pad = nullptr;
    if (camera->Mode() != 4 || camera->mPadID >= 4) {
        return HOOK_CONTINUE;
    }

    auto& pad = mDoCPd_c::getCpadInfo(camera->mPadID);
    s_cameraInput.pad = &pad;
    s_cameraInput.subStick = read_sub_stick(pad);
    write_sub_stick(pad, {});
    return HOOK_CONTINUE;
}

void on_camera_update_pad_post(ModContext*, void*, void*, void*) {
    if (s_cameraInput.pad == nullptr) {
        return;
    }

    write_sub_stick(*s_cameraInput.pad, s_cameraInput.subStick);
    s_cameraInput.pad = nullptr;
}

struct LinkLookInput {
    daAlink_c* link = nullptr;
    f32 moveValue = 0.0f;
    s16 stickAngle = 0;
};

LinkLookInput s_linkLookInput;

HookAction on_link_camera_look_pre(ModContext*, void* args, void*, void*) {
    auto* link = mods::arg<daAlink_c*>(args, 0);
    s_linkLookInput.link = nullptr;
    if (link->mProcID != daAlink_c::PROC_SUBJECTIVITY) {
        return HOOK_CONTINUE;
    }

    s_linkLookInput.link = link;
    s_linkLookInput.moveValue = link->mMoveValue;
    s_linkLookInput.stickAngle = link->mStickAngle;
    link->mMoveValue = mDoCPd_c::getSubStickValue(PAD_1);
    link->mStickAngle = static_cast<s16>(
        static_cast<u16>(mDoCPd_c::getSubStickAngle(PAD_1)) + 0x8000u);
    return HOOK_CONTINUE;
}

void on_link_camera_look_post(ModContext*, void*, void*, void*) {
    if (s_linkLookInput.link == nullptr) {
        return;
    }

    s_linkLookInput.link->mMoveValue = s_linkLookInput.moveValue;
    s_linkLookInput.link->mStickAngle = s_linkLookInput.stickAngle;
    s_linkLookInput.link = nullptr;
}

void on_link_subjectivity_post(ModContext*, void* args, void*, void*) {
    auto* link = mods::arg<daAlink_c*>(args, 0);
    if (link->mProcID != daAlink_c::PROC_SUBJECTIVITY || link->checkEventRun() || link->mpHIO == nullptr) {
        return;
    }

    const f32 maxSpeed = link->checkWolf()
        ? link->mpHIO->mWolf.mWlMove.m.mMaxSpeed
        : link->mpHIO->mMove.m.mMaxSpeed;

    link->current.angle.y = link->mMoveAngle;
    link->mNormalSpeed = link->mStickValue * maxSpeed;
}

}  // namespace

extern "C" {
MOD_EXPORT ModResult mod_initialize(ModError*) {
    ModResult result = mods::hook::add_post<LinkSubjectivity>(on_link_subjectivity_post);
    if (result != MOD_OK) {
        mods::log::error("failed to install first-person movement post-hook");
        return result;
    }

    result = mods::hook::add_pre<LinkCameraLook>(on_link_camera_look_pre);
    if (result != MOD_OK) {
        mods::hook::uninstall<LinkSubjectivity>();
        mods::log::error("failed to install first-person camera look pre-hook");
        return result;
    }

    result = mods::hook::add_post<LinkCameraLook>(on_link_camera_look_post);
    if (result != MOD_OK) {
        mods::hook::uninstall<LinkCameraLook>();
        mods::hook::uninstall<LinkSubjectivity>();
        mods::log::error("failed to install first-person camera look post-hook");
        return result;
    }

    result = mods::hook::add_pre<CameraUpdatePad>(on_camera_update_pad_pre);
    if (result != MOD_OK) {
        mods::hook::uninstall<LinkCameraLook>();
        mods::hook::uninstall<LinkSubjectivity>();
        mods::log::error("failed to install first-person camera exit-gesture pre-hook");
        return result;
    }

    result = mods::hook::add_post<CameraUpdatePad>(on_camera_update_pad_post);
    if (result != MOD_OK) {
        mods::hook::uninstall<CameraUpdatePad>();
        mods::hook::uninstall<LinkCameraLook>();
        mods::hook::uninstall<LinkSubjectivity>();
        mods::log::error("failed to install first-person camera exit-gesture post-hook");
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
