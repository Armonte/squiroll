#pragma once

#ifndef Actor2D_H
#define Actor2D_H 1

// Companion notes for the rollback work-in-progress.
//
// The C++ struct layouts for Manbow::Actor2D, Manbow::Actor2DGroup, and the
// animation controller chain already live in TF4.h (search for
// `struct ManbowActor2D`, `ManbowActor2DGroup`, `ManbowAnimationController2D`,
// `TakeData`, `AnimationData`, etc). zero318's ClangAsmTest1 sandbox at
// AoCF/aocf_test.cpp duplicates the same offsets under slightly different
// names (`Actor2D`, `Actor2DGroup`, `Actor2DManager`). Both agree on size
// 0xEC and the field layout.
//
// What's missing from those two sources and *not* yet captured anywhere is:
//   1. The full Sqrat method binding table (this file)
//   2. The lifecycle sequence rollback must integrate with (this file)
//   3. The DerivedClass implementation details (this file)
//   4. Where the 5 "flag" SQObjects actually live (this file)
//
// Verified by decompiling the four Sqrat bind-site functions:
//   sub.Actor2DSetup         @ Rx94 4D0   3503 bytes
//   sub.Actor2DGroupSetup    @ Rx9A 660   692 bytes
//   sub.Actor2DManagerSetup  @ Rx9D BD0   596 bytes
//   sub.Actor2DProcGroupSetup @ RxA21A0   278 bytes
//
// All RVAs below are in the canonical scheme (image base 0x400000) so they
// drop straight into squiroll's `_R` macro. If you're reading our IDA db,
// add 0x260000.

#include "TF4.h"   // ManbowActor2D, ManbowActor2DGroup, ManbowAnimationController2D, ...

// ---------------------------------------------------------------------------
// Bound methods, grouped by purpose
// ---------------------------------------------------------------------------
//
// All take `this = ManbowActor2D*` via __thiscall unless noted. The address
// is the Sqrat-binding wrapper, which may tail-call a deeper implementation
// — these are the right addresses to hook for behavioral overrides.
//
// Lifecycle:
//   Release                          0x121230_R
//   Update                           0x121280_R
//   CreateActor2D                    0x121650_R   Manbow::Actor2D::CreateActor2D_
//                                                 (forwards to this->actor2d_mgr->CreateActor2D)
//   CreateActor2DTrail               0x1216A0_R
//   CreateActor2DStencil             0x1216F0_R
//   CreateActor2DDynamic             0x121740_R
//   CreateActor3D                    0x121790_R   Manbow::Actor2D::CreateActor2DTrail (sic, shared impl)
//   SetParent                        0x0F5BB0_R
//   ConnectRenderSlot                0x122650_R
//
// Class-definition helpers (used in actor.nut::DefinePlayerClass):
//   DerivedClass                     0x0F5280_R   → sub_6F5D60 (impl below)
//   BindTakeVariable                 0x0F5430_R
//   BindFrameFlag                    0x0F5520_R
//   BindFrameVariable                0x0F5620_R
//   DefineStaticVariable             0x0F52B0_R
//
// Animation:
//   SetMotion                        0x1229F0_R
//   SetTake                          0x122A00_R
//   SetKeyFrame                      0x122A10_R
//   GetKeyFrameData                  0x122A20_R
//   Warp                             0x1215D0_R   Manbow::Actor2D::Warp
//   Warp3D                           0x121610_R
//   SetCollisionRotation             0x122A80_R
//   SetCollisionScaling              0x122A40_R
//   GetPoint                         0x0F42D0_R
//   GetPointLocal                    0x0F4420_R
//
// Callback wiring (the Ex variants take an extra "id" arg for multi-binding):
//   SetUpdateFunction                0x0F3E10_R   writes ManbowActor2D::update_func @ 0xA8
//   SetUpdateFunctionEx              0x0F5860_R
//   SetEndTakeCallbackFunction       0x0F3EB0_R
//   SetEndTakeCallbackFunctionEx     0x0F5880_R
//   SetEndMotionCallbackFunction     0x0F3FF0_R
//   SetEndMotionCallbackFunctionEx   0x0F5A30_R
//   SetContactTestCallbackFunction   0x0F3F50_R   writes ManbowActor2D::__sqrat_funcBC @ 0xBC
//   SetContactTestCallbackFunctionEx 0x0F5B90_R
//   SetCallbackOnScreen              0x0F4090_R
//   SetCallbackOnScreenEx            0x11BB80_R
//   SetCallbackReflection            0x11BD30_R
//   SetCallbackCount                 0x11B880_R
//   SetCallbackRepeat                0x11BA00_R
//   SetCallbackSpeedX                0x11C330_R
//   SetCallbackSpeedY                0x11C4C0_R
//   SetCallbackScalingX              0x11C650_R
//   SetCallbackScalingY              0x11C7E0_R
//   SetCallbackRotationX             0x11C970_R
//   SetCallbackRotationY             0x11CB00_R
//   SetCallbackRotationZ             0x11CC90_R
//   SetCallbackColorA                0x11CE20_R
//   SetCallbackColorR                0x11CFB0_R
//   SetCallbackColorG                0x11D140_R
//   SetCallbackColorB                0x11D2D0_R
//
// Task management (per-frame deferred actions on this actor):
//   SetTaskAddSpeed                  0x121E40_R
//   SetTaskAddRotation               0x121F40_R
//   SetTaskAddScaling                0x121EC0_R
//   SetTaskAddColor                  0x121FD0_R
//   SetTaskAutoRotation              0x122080_R
//   SetTaskAutoCollisionUpdate       0x1220F0_R
//   RemoveTask                       0x121DD0_R
//   ClearTask                        0x121DC0_R

// ---------------------------------------------------------------------------
// Manbow.Actor2DGroup methods (collision/processing group)
// ---------------------------------------------------------------------------
//   Update                            0x0FB280_R
//   Refresh                           0x0FB120_R
//   GetSize                           0x0FB110_R
//   Clear                             0x0FAFB0_R
//   ClearAll                          0x0FB100_R
//   CacheContactTest                  0x0FB690_R
//   ContactTest                       0x0FB960_R
//   SetWorld                          0x0FABD0_R
//   SetCamera                         0x0FACD0_R
//   SetUpdateType                     0x0FB230_R
//   SetUpdateMask                     0x0FB270_R
//   SetOnMoveCallbackFunction         0x0FBC80_R
//   SetOnHitActorCallbackFunction     0x0FBC60_R
//   SetOnHitCollisionCallbackFunction 0x0FBC70_R
//   Move                              0x0FB550_R
//   LoopVertical                      0x0FB610_R

// ---------------------------------------------------------------------------
// Manbow.Actor2DManager methods (per-character actor pool)
// ---------------------------------------------------------------------------
//   SetGroup                          0x0FE140_R
//   Update                            0x0FF4E0_R
//   LoadAnimationData                 0x0FEB50_R
//   CreateAnimationData               0x0FF3A0_R
//   GetAnimationSet2D                 0x0FF460_R
//   LoadMesh                          0x0FF3C0_R
//   CreateActor2D                     0x0FE340_R   Manbow::Actor2DManager::CreateActor2D
//   CreateActor2DTrail                0x0FE470_R
//   CreateActor2DStencil              0x0FE610_R
//   CreateActor2DDynamic              0x0FE7B0_R
//   CreateActor3D                     0x0FE950_R
//   CreateAnimationController2D       0x0FEA80_R
//   GetGlobalAnimation                0x0FF3E0_R
//
// All of the Manager::CreateActor2D* functions are the *real* factories;
// Manbow::Actor2D::CreateActor2D_ (0x121650_R) just forwards via
// this->actor2d_mgr. So if you want to intercept actor birth, hook
// Manager::CreateActor2D — there's one Manager per character per team.

// ---------------------------------------------------------------------------
// Manbow.Actor2DProcGroup methods
// ---------------------------------------------------------------------------
//   Clear   0x1022C0_R
//   Add     0x1022D0_R
//   Foreach 0x102500_R
//   Move    0x102530_R

// ---------------------------------------------------------------------------
// Discrepancy: bottom and vy
// ---------------------------------------------------------------------------
// TF4.h and aocf_test.cpp both mark these as float, but the Sqrat binding
// in sub.Actor2DSetup registers them via the *int* helpers:
//   bottom (@ 0x28)  ←  sub_6F78A0  (getter: sq_pushinteger)
//   vy     (@ 0x30)  ←  sub_6F79B0  (getter: sq_pushinteger)
// The underlying memory load is a raw 4-byte read — same instruction for
// both — so the bits don't change. But Squirrel will see them as ints,
// not floats. Either the existing struct definitions are wrong, or these
// fields are used as int-flags in some code paths while still being read
// as floats elsewhere (e.g. directly from the C++ side bypassing the
// Squirrel binding). Worth a closer look when finalizing the layout.

// ---------------------------------------------------------------------------
// DerivedClass implementation (sub_6F5D60)
// ---------------------------------------------------------------------------
//
//   sq_newclass(v, /*hasbase=*/0);       // a NEW class, NOT inheriting Actor2D
//   sq_getstackobj(v, -1, &out);
//   sq_addref(v, &out);
//   sq_settypetag(v, -1, <Actor2D type tag>);  // still tagged as Actor2D
//   foreach static-slot in Actor2D's class:
//       copy slot into the new class
//   bind metamethods: _set (sqVarSet), _get (sqVarGet), weakref, _typeof
//   return out;
//
// Key takeaway: the derived class shares Actor2D's *type tag* (so the
// C++ side, Sqrat::ClassType<Actor2D>, treats instances as Actor2D-
// compatible), but it does NOT inherit Actor2D's members directly. The
// script side then assigns members onto the empty class.
//
// actor.nut::DefinePlayerClass calls DerivedClass exactly 4 times per
// character (player_class, shot_class, collision_object_class,
// player_effect_class), so there are 8 unique derived classes per match
// (one set per team). They're disposed when the match ends — rollback
// only needs to track *instances* of these classes, not the classes
// themselves.

// ---------------------------------------------------------------------------
// The 5 flag variables (flag1..flag5)
// ---------------------------------------------------------------------------
//
// Declared in actor_member.nut:48-52 (`this.flag1 <- 0` ... `this.flag5 <- 0`).
// They live in the SQInstance's _values[] array (per-instance Squirrel
// storage), NOT in the C++ ManbowActor2D struct above. ManbowActor2D::sq_obj
// (offset 0x74) is one SQObject — almost certainly the back-pointer to the
// owning SQInstance, not the flags.
//
// Per-move usage examples:
//   com_update.nut:64-65       t_.atkRank <- this.flag1;  t_.atk <- this.flag2;
//   player_update.nut:52-53    same
//   Koishi LW (per Dazegambler): hitbox table stored in one of the flags
//
// rollback.cpp's getTemplate() (in message(4).txt) only walks keys whose
// type is integer/float/string/bool/array. A flag holding an OT_TABLE (the
// Koishi-LW case) will NOT be snapshotted. Fix options:
//   (a) Hard-code flag1..flag5 in getTemplate() and deep-clone their value
//       (handle OT_TABLE / OT_INSTANCE / OT_ARRAY of mixed types).
//   (b) Leverage rollback.cpp's already-installed SQVM_set_hook to capture
//       per-frame diffs on these slots — but the existing diff scheme
//       stores the SQObject by sq_addref, which is fine for tables but
//       won't preserve table *contents* if those mutate too.
// (a) is simpler; (b) is more correct but recursive.

// ---------------------------------------------------------------------------
// Lifecycle sequence rollback must integrate with
// ---------------------------------------------------------------------------
//
// Allocation (Sqrat::SharedPoolAllocator<Manbow::Actor2D>):
//   - Actor2D instances are drawn from a shared pool, NOT per-actor heap.
//   - The pool's release-hook is sub_6F6B50 (registered in Sqrat::Class::
//     Actor2D at 0x0F6280_R).
//   - To survive a rollback that resurrects a "released" actor, the pool
//     slot must remain reserved until the rollback window passes. The
//     easiest hook: redirect the Release wrapper (0x121230_R) into a
//     deferred-free queue, and only run the original Release on commit.
//
// Release (0x121230_R, sub_721230):
//   actor->active_flags = 4;               // 0x70 byte = "released"
//   actor->actor2d_group->pending_release = 1;   // group flag @ 0x28
//   sub_722AC0(actor);                      // walk task vector @ 0x9C-0xA0,
//                                           // release each task via
//                                           // task->vtbl[+0x24]
//
// Snapshot strategy:
//   1. Per-frame: memcpy of each live Actor2D struct (0xEC bytes) is
//      cheap and complete for the C++ side.
//   2. The anim_controller (shared_ptr @ 0x3C) shares state across actors
//      in some cases — snapshot via shared_ptr increment, NOT memcpy.
//   3. update_func (@ 0xA8) and __sqrat_funcBC (@ 0xBC) hold SqratFunction
//      objects that own an HSQOBJECT. Snapshot via sq_addref on the
//      embedded SQObject; sq_release on commit/rollback.
//   4. The task list (vector @ 0x9C-0xA0 and list @ 0xD0) needs per-task
//      snapshot — tasks have their own vtables and state.
//   5. ManbowActor2DGroup's actor_list / actor_vec (@ 0xC and 0x14) are
//      the authoritative live-set; rollback needs to restore both the
//      pointer arrays AND each actor's state.

#endif // Actor2D_H
