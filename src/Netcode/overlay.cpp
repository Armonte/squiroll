
#include <cstddef>
#include <cstdint>
#include <cstdio>
#if __INTELLISENSE__
#undef _HAS_CXX20
#define _HAS_CXX20 0
#endif

#include <d3d11.h>
#include <vector>
#include <memory>
#include <list>

#include "config.h"
#include "log.h"
#include "squirrel.h"
#include "util.h"
#include "overlay.h"

#include "D3D.h"
#include "TF4.h"
#include "bt.h"
#include "sqrat.h"
#include "d3d_probe.h"    // [#28] region-A D3D resource probe
#include "render_slot.h"  // [#30] squiroll-owned plugin HUD draw slot
#include "hud_font8x8.h"  // [#30 native HUD] embedded 8x8 bitmap font (CC0)

static constexpr uint8_t hitbox_vert_cso[] = {
#include "shaders/hitbox_vert.cso.h"
};
static constexpr uint8_t hitbox_frag_cso[] = {
#include "shaders/hitbox_frag.cso.h"
};

// Quad budget for the shared vertex/index/structured buffers (verts = *4,
// indices = *6, structured elems = *1). Bumped 1024 -> 4096 for the native HUD:
// text expands to many small rects (each lit font-pixel is one quad), so a full
// stats line can be ~1000+ quads on top of the debug hitboxes. All buffer
// allocations below scale off this, and both the hitbox and HUD emit paths cap
// gracefully at MAX_HITBOXES (no overflow).
#define MAX_HITBOXES 4096

#define d3d11_dev ((ID3D11Device**)0x4DAE9C_R)
#define d3d11_imm ((ID3D11DeviceContext**)0x4DAE98_R)
#define btBox2dShape_getHalfExtentsWithMargin ((btVector3* (*thiscall)(btCollisionShape*, btVector3*))0x1C0EB0_R)
//#define draw_rect ((void (vectorcall*)(const float*, float, float, float, float))0x39600_R)

enum DrawType : uint8_t {
    DRAW_RECT,
    DRAW_CIRCLE,
};

struct DrawObj {
    DrawType prim;
    uint16_t group;
    union {
        union {
            float points[2 * 4];
            vec<float, 4> points_vec[2];
        } rect;
        union {
            float aabb[2 * 2];
            vec<float, 4> aabb_vec;
        } circle;
    };

    // This avoids calling memset when resizing vectors
    inline DrawObj() {}
};

struct HitboxVertex {
    float pos[2];
};

// Constant buffer sizes have to be 16-byte aligned
struct alignas(16) HitboxConstantBuffer {
    float alphas[2];
};

struct HitboxGPUData {
    uint32_t col;
    uint32_t flags;
    float thresholds[2];
};

static bool overlay_supported = true;

static ID3D11VertexShader* hitbox_vs = nullptr;
static ID3D11PixelShader* hitbox_ps = nullptr;
static ID3D11InputLayout* hitbox_il = nullptr;
static ID3D11Buffer* hitbox_vb = nullptr;
static ID3D11Buffer* hitbox_ib = nullptr;
static ID3D11Buffer* hitbox_sb = nullptr;
static ID3D11ShaderResourceView* hitbox_sb_srv = nullptr;
static ID3D11Buffer* hitbox_cb = nullptr;

#define CHECK_RES(x) \
    res = x; \
    if (FAILED(res)) { \
        mboxf("D3D11 Exception", MB_OK | MB_ICONERROR, [=](auto add_text) { \
            add_text(#x "\nfailed with result 0x%08X", res); \
        }); \
    }
void overlay_init() {
    HRESULT res;
    ID3D11Device* dev = *d3d11_dev;

    if (dev->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
        log_printf("D3D11 feature level is too low to support the overlay!\n");
        overlay_supported = false;
        return;
    }

    CHECK_RES(dev->CreateVertexShader(hitbox_vert_cso, sizeof(hitbox_vert_cso), nullptr, &hitbox_vs));
    CHECK_RES(dev->CreatePixelShader(hitbox_frag_cso, sizeof(hitbox_frag_cso), nullptr, &hitbox_ps));

    static constexpr D3D11_INPUT_ELEMENT_DESC input_layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    CHECK_RES(dev->CreateInputLayout(input_layout, countof(input_layout), hitbox_vert_cso, sizeof(hitbox_vert_cso), &hitbox_il));

    static constexpr D3D11_BUFFER_DESC vertex_desc = {
        .ByteWidth = sizeof(HitboxVertex) * MAX_HITBOXES * 4,
        .Usage = D3D11_USAGE_DYNAMIC,
        .BindFlags = D3D11_BIND_VERTEX_BUFFER,
        .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
        .MiscFlags = 0,
        .StructureByteStride = 0,
    };
    CHECK_RES(dev->CreateBuffer(&vertex_desc, NULL, &hitbox_vb));

    uint16_t* index_vec = new uint16_t[MAX_HITBOXES * 6];
    for (size_t i = 0; i < MAX_HITBOXES; i++) {
        index_vec[i * 6] = i * 4;
        index_vec[i * 6 + 1] = i * 4 + 1;
        index_vec[i * 6 + 2] = i * 4 + 2;
        index_vec[i * 6 + 3] = i * 4;
        index_vec[i * 6 + 4] = i * 4 + 2;
        index_vec[i * 6 + 5] = i * 4 + 3;
    }
    static constexpr D3D11_BUFFER_DESC index_desc = {
        .ByteWidth = MAX_HITBOXES * 6 * sizeof(*index_vec),
        .Usage = D3D11_USAGE_IMMUTABLE,
        .BindFlags = D3D11_BIND_INDEX_BUFFER,
        .CPUAccessFlags = 0,
        .MiscFlags = 0,
        .StructureByteStride = 0,
    };
    D3D11_SUBRESOURCE_DATA index_init = {
        .pSysMem = index_vec,
    };
    CHECK_RES(dev->CreateBuffer(&index_desc, &index_init, &hitbox_ib));
    delete[] index_vec;

    static constexpr D3D11_BUFFER_DESC struct_desc = {
        .ByteWidth = sizeof(HitboxGPUData) * MAX_HITBOXES,
        .Usage = D3D11_USAGE_DYNAMIC,
        .BindFlags = D3D11_BIND_SHADER_RESOURCE,
        .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
        .MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
        .StructureByteStride = sizeof(HitboxGPUData),
    };
    CHECK_RES(dev->CreateBuffer(&struct_desc, NULL, &hitbox_sb));

    static constexpr D3D11_SHADER_RESOURCE_VIEW_DESC struct_srv = {
        .Format = DXGI_FORMAT_UNKNOWN,
        .ViewDimension = D3D11_SRV_DIMENSION_BUFFER,
        .Buffer = {
            .FirstElement = 0,
            .NumElements = MAX_HITBOXES,
        }
    };
    CHECK_RES(dev->CreateShaderResourceView(hitbox_sb, &struct_srv, &hitbox_sb_srv));

    static constexpr D3D11_BUFFER_DESC constant_desc = {
        .ByteWidth = sizeof(HitboxConstantBuffer),
        .Usage = D3D11_USAGE_DYNAMIC,
        .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
        .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
        .MiscFlags = 0,
        .StructureByteStride = 0,
    };
    CHECK_RES(dev->CreateBuffer(&constant_desc, NULL, &hitbox_cb));
}
#undef CHECK_RES

#define SAFE_RELEASE(x) \
    if (x) { \
        x->Release(); \
        x = nullptr; \
    }
void overlay_destroy() {
    SAFE_RELEASE(hitbox_vs);
    SAFE_RELEASE(hitbox_ps);
    SAFE_RELEASE(hitbox_il);
    SAFE_RELEASE(hitbox_vb);
    SAFE_RELEASE(hitbox_ib);
    SAFE_RELEASE(hitbox_sb);
    SAFE_RELEASE(hitbox_sb_srv);
    SAFE_RELEASE(hitbox_cb);
}
#undef SAFE_RELEASE

static std::vector<DrawObj> overlay_collision;
static std::vector<DrawObj> overlay_hurt;
static std::vector<DrawObj> overlay_hit;

static forceinline void transform_vec2(float* dst, const float* src, const D3DMATRIX* mat) {
    dst[0] = mat->m[0][0] * src[0] + mat->m[1][0] * src[1] + mat->m[3][0];
    dst[1] = mat->m[0][1] * src[0] + mat->m[1][1] * src[1] + mat->m[3][1];
}

/*
static forceinline void transform_vec4(float* dst, const float* src, const D3DMATRIX* mat) {
    dst[0] = mat->m[0][0] * src[0] + mat->m[1][0] * src[1] + mat->m[3][0];
    dst[1] = mat->m[0][1] * src[0] + mat->m[1][1] * src[1] + mat->m[3][1];
    dst[2] = mat->m[0][0] * src[2] + mat->m[1][0] * src[3] + mat->m[3][0];
    dst[3] = mat->m[0][1] * src[2] + mat->m[1][1] * src[3] + mat->m[3][1];
}
*/

static forceinline vec<float, 4> transform_vec4(vec<float, 4> src, vec<float, 4> m0, vec<float, 4> m1, vec<float, 4> m3) {
    vec<float, 4> src_even = shufflevec(src, src, 0, 0, 2, 2);
    vec<float, 4> src_odd = shufflevec(src, src, 1, 1, 3, 3);

    return m0 * src_even + m1 * src_odd + m3;
}

static forceinline void transform_vec2(float* dst, float x, float y, const btTransform* mat) {
    dst[0] = mat->basis.m[0].v[0] * x + mat->basis.m[0].v[1] * y + mat->origin.v[0];
    dst[1] = mat->basis.m[1].v[0] * x + mat->basis.m[1].v[1] * y + mat->origin.v[1];
}

/*
static forceinline void transform_vec4(float* dst, float x, float y, const btTransform* mat) {
    dst[0] = mat->basis.m[0].v[0] * x + mat->basis.m[0].v[1] * y + mat->origin.v[0];
    dst[1] = mat->basis.m[1].v[0] * x + mat->basis.m[1].v[1] * y + mat->origin.v[1];
    dst[2] = mat->basis.m[0].v[0] * -x + mat->basis.m[0].v[1] * y + mat->origin.v[0];
    dst[3] = mat->basis.m[1].v[0] * -x + mat->basis.m[1].v[1] * y + mat->origin.v[1];
}
*/

static forceinline vec<float, 4> transform_vec4(vec<float, 4> basisX, vec<float, 4> basisY, vec<float, 4> origin) {
    return basisX + basisY + origin;
}

static void get_boxes(std::vector<DrawObj>& dst, const std::vector<std::shared_ptr<ManbowActorCollisionData>>& src, const ManbowCamera2D* camera) {
    union {
        btVector3 rect_extents;
        btVector3 aabb[2];
    };

    size_t prev_size = dst.size();
    dst.resize(prev_size + src.size());
    DrawObj* draw_ptr = &dst[prev_size];

    vec<float, 4> view_proj0 = shufflevec(camera->view_proj_matrix.m[0], camera->view_proj_matrix.m[0], 0, 1, 0, 1);
    vec<float, 4> view_proj1 = shufflevec(camera->view_proj_matrix.m[1], camera->view_proj_matrix.m[1], 0, 1, 0, 1);
    vec<float, 4> view_proj3 = shufflevec(camera->view_proj_matrix.m[3], camera->view_proj_matrix.m[3], 0, 1, 0, 1);

    for (const auto& data : src) {
        btGhostObject* obj = data->obj_ptr;

        draw_ptr->group = obj->m_broadphaseProxy ? obj->m_broadphaseProxy->m_collisionFilterGroup : 0;

        switch (obj->m_collisionShape->shape) {
            case 17: {
                btBox2dShape_getHalfExtentsWithMargin(obj->m_collisionShape, &rect_extents);
                draw_ptr->prim = DRAW_RECT;
                //draw.prim = DRAW_RECT;

                // The vertices get transformed on the CPU to calculate the border thresholds later
                // TODO: It might be faster to multiply the two matrices together and use that instead
                
                /*
                float world_points[4];
                transform_vec2(&world_points[0], -rect_extents.v[0], -rect_extents.v[1], &obj->m_worldTransform);
                transform_vec2(&world_points[2], rect_extents.v[0], -rect_extents.v[1], &obj->m_worldTransform);
                transform_vec2(&draw.rect.points[0], &world_points[0], &camera->view_proj_matrix);
                transform_vec2(&draw.rect.points[2], &world_points[2], &camera->view_proj_matrix);
                transform_vec2(&world_points[0], rect_extents.v[0], rect_extents.v[1], &obj->m_worldTransform);
                transform_vec2(&world_points[2], -rect_extents.v[0], rect_extents.v[1], &obj->m_worldTransform);
                transform_vec2(&draw.rect.points[4], &world_points[0], &camera->view_proj_matrix);
                transform_vec2(&draw.rect.points[6], &world_points[2], &camera->view_proj_matrix);
                */

                /*
                float world_points[4];
                transform_vec4(&world_points[0], -rect_extents.v[0], -rect_extents.v[1], &obj->m_worldTransform);
                transform_vec4(&draw.rect.points[0], &world_points[0], &camera->view_proj_matrix);
                transform_vec4(&world_points[0], rect_extents.v[0], rect_extents.v[1], &obj->m_worldTransform);
                transform_vec4(&draw.rect.points[4], &world_points[0], &camera->view_proj_matrix);
                */
                
                vec<float, 4> world_origin = shufflevec(obj->m_worldTransform.origin.v, obj->m_worldTransform.origin.v, 0, 1, 0, 1);
                vec<float, 4> world_basisX = shufflevec(obj->m_worldTransform.basis.m[0].v, obj->m_worldTransform.basis.m[1].v, 0, 4, 0, 4);
                vec<float, 4> world_basisY = shufflevec(obj->m_worldTransform.basis.m[0].v, obj->m_worldTransform.basis.m[1].v, 1, 5, 1, 5);
                vec<float, 4> extentsX = { -rect_extents.v[0], -rect_extents.v[0], rect_extents.v[0], rect_extents.v[0] };
                vec<float, 4> extentsY = { rect_extents.v[1], rect_extents.v[1], rect_extents.v[1], rect_extents.v[1] };

                vec<float, 4> world_points[2];
                world_points[0] = transform_vec4(world_basisX * extentsX, world_basisY * -extentsY, world_origin);
                world_points[1] = transform_vec4(world_basisX * -extentsX, world_basisY * extentsY, world_origin);

                draw_ptr->rect.points_vec[0] = transform_vec4(world_points[0], view_proj0, view_proj1, view_proj3);
                draw_ptr->rect.points_vec[1] = transform_vec4(world_points[1], view_proj0, view_proj1, view_proj3);
                break;
            }
            case 8: {
                obj->m_collisionShape->getAabb(&obj->m_worldTransform, &aabb[0], &aabb[1]);
                draw_ptr->prim = DRAW_CIRCLE;
                //transform_vec2(&draw.circle.aabb[0], (float*)&aabb[0].v, &camera->view_proj_matrix);
                //transform_vec2(&draw.circle.aabb[2], (float*)&aabb[1].v, &camera->view_proj_matrix);
                vec<float, 4> aabb_vec = { aabb[0].v[0], aabb[0].v[1], aabb[1].v[0], aabb[1].v[1] };
                draw_ptr->circle.aabb_vec = transform_vec4(aabb_vec, view_proj0, view_proj1, view_proj3);
                break;
            }
            default:
                log_printf("Unhandled hitbox shape %u\n", obj->m_collisionShape->shape);
                break;
        }
        ++draw_ptr;
    }
}

static size_t hitbox_write_idx = 0;
static HitboxVertex* hitbox_vertices = nullptr;
static HitboxGPUData* hitbox_gpu_data = nullptr;
static int hitbox_player_flags[2] = {};
void overlay_set_hitboxes(ManbowActor2DGroup* group, int p1_flags, int p2_flags) {
    overlay_clear();
    if (!get_hitbox_vis_enabled()) {
        return;
    }

    ManbowCamera2D* camera = group->camera;
    hitbox_player_flags[0] = p1_flags;
    hitbox_player_flags[1] = p2_flags;

    if (uint32_t group_size = group->size) {
        ManbowActor2D** actor_ptr = group->actor_vec.data();
        do {
            ManbowActor2D* actor = *actor_ptr++;
            if (!actor->anim_controller || (actor->active_flags & 1) == 0 || (actor->group_flags & group->update_mask) == 0 || !actor->callback_group)
                continue;

            get_boxes(overlay_collision, actor->anim_controller->collision_boxes, camera);
            get_boxes(overlay_hurt, actor->anim_controller->hurt_boxes, camera);
            get_boxes(overlay_hit, actor->anim_controller->hit_boxes, camera);

        } while (--group_size);

    }
}

// int _SetTake(ManbowAnimationController2D* self,int32_t take){
//     while (self->take->previous){
//         self->take = self->take->previous;
//     }
//     self->key_take = 0;
//     for(int32_t i = 0; i < take; ++i){
//         self->take = self->take->next;
//         self->key_take = i;
//     }
//     self->__vec224.clear();
//     self->__vec224.resize(self->take->__vec14.size());
//     take = 0;
//     while(take < self->__vec224.size()){
//         self->__vec224[take] = self->take->__vec14[take];
//         //then deletes that element on the take vector
//     }
//     self->vftable->__method20(0);
//     return 1;
// }

// bool PlayTake(ManbowAnimationController2D* self, int32_t keyframe) {
//     TakeData* take = self->take;
//     if (!take){
//         return false;
//     }
//     self->keyframe = keyframe;
//     self->animation_data = &take->frame_data[keyframe];
//     int32_t frame = 0;
//     if(keyframe){
//         frame  = take->frame_data[keyframe - 1].frame_total;
//     }
//     self->frame = frame;
//     self->frame_again = frame;
//     self->vftable->__method8C();
//     return true;
// }

// void ManbowAnimationController2D::Loop(){
//     if(this->animation_data){
//         this->frame += this->speed;
//         this->frame_again += this->speed;
//         if(this->animation_data->frame_total <= this->frame_again){
//             this->vftable->__methodA0();
//         }
//         for (int32_t i = 0; i < this->sprites.size()){
//             this->sprites[i]->vtable->__Method8(this->keyframe,this->frame);
//         }
//     }
//     return;
// }

// void ManbowAnimationController2D::7C(){
//     if(this->animation_data && this->__unk11C){
//         // some global bool is set to this->__bool13E
//         for (std::shared_ptr<Sprite> sprite : this->sprites){
//             sprite->vtable->vtable->__Method10(this->red,this->__bool13E);
//         }
        
//         if (this->__bool13E){
//             for (int32_t i = 0; i < this->animation_data->__int4b){
//                 this->animation_data[i].vtable->__MethodC(this->__matrix_38,this->red);
//             }
//         }else {
//             for (int32_t i = 0; i < this->animation_data->__int4b){
//                 D3DMATRIX* matrix = this->animation_data[i]->vtable->Method8();
//                 float a = this->red * matrix->m[0][];
//                 float b = this->green * matrix->m[1][];
//                 float c = this->blue * matrix->m[2][];
//                 float d = this->alpha * matrix->m[3][];
//                 this->animation_data[i].vtable->__MethodC(this->__matrix_38,a);
//             }
//         }
//     }
// }

// D3DMATRIX ManbowAnimationController2D::90(D3DMATRIX matrix,int32_t take) {
//     if (this->sprites.size() <= take) {
//         float r0[4]; //these are global variables but
//         float r1[4]; //for simplicity they're local here
//         float r2[4];
//         float r3[4];
//         box_data->matrix[0] = r0;
//         box_data->matrix[1] = r1;
//         box_data->matrix[2] = r2;
//         box_data->matrix[3] = r3;
//     }
//     this->sprites[take]->vtable->__method18(matrix);
// }

// void ManbowAnimationController2D::A0() {
//     TakeData* take = this->take;
//     while(take->frame_total <= this->frame_again){
//         Unk3 new_data;
//         if(++this->keyframe < take->__int24){
//             new_data = take->frame_data[this->keyframe];
//         }else{
//             if (!take->__bool25){
//                 this->vftable->__method9C();
//                 return;
//             }
//             this->keyframe = 0;
//             this->frame_again = this->frame_again % take->frame_total;
//             new_data = &take->frame_data;
//         }
//         this->animation_data = &new_data;
//     }
//     this->vftable->__method8C();
// }

// void ManbowAnimationController2D::SetBoxes(){
//     if(!this->animation_data)return;
//     void* btsetboxidk(
//         BoxData,
//         int32_t,
//         std::vector<std::shared_ptr<ManbowActorCollisionData>>,
//         Unk140*
//     );
//     btsetboxidk(
//         this->animation_data->box_data[0],
//         this->animation_data->col_count,
//         this->collision_boxes,
//         this->__unk140
//     );
//     btsetboxidk(
//         this->animation_data->box_data[this->animation_data->hurt_count],
//         this->animation_data->hit_count - this->animation_data->hurt_count,
//         this->hit_boxes,
//         this->__unk140->__ptr40
//     );
//     btsetboxidk(
//         this->animation_data->box_data[this->animation_data->col_count],
//         this->animation_data->hurt_count - this->animation_data->col_count,
//         this->hurt_boxes,
//         this->__unk140->__ptr40
//     );
//     if(this->__unkC4)this->__unkC4->__method8();
// }

void dump_framedata(AnimationData* anim_data) {
    log_printf(
    "\nAnimationData {\n"
    "    frame_total = %df(true value:%d)\n"
    "    flags = {0x%p,0x%p}\n"
    "    data {\n"
    "        damage = %d\n"
    "        hitStopE = %d\n"
    "        hitStopP = %d\n"
    "        guardStopE = %d\n"
    "        guardStopP = %d\n"
    "        firstRate = %d\n"
    "        comboRate = %d\n"
    "        AddKnock_Unk = %d\n"
    "        stun = %d\n"
    "        bariaBreak_Unk = %d\n"
    "        guardRealDamage = %d\n"
    "        slaveBlockOccult = %d\n"
    "        SCGauge_onhit = %d\n"
    "        comboRecoverTime = %d\n"
    "        minRate_unused = %d\n"
    "        stopVecX = %d\n"
    "        stopVecY = %d\n"
    "        hitSoundEffect = %d\n"
    "        hitVecX = %d\n"
    "        hitVecY = %d\n"
    "        grazeKnock_Unk = %d\n"
    "        atkType = %d\n"
    "        atkRank = %d\n"
    "        hitEffectVFX = %d\n"
    "    }\n"
    "    col_count = %d\n"
    "    hurt_count = %d\n"
    "    hit_count = %d\n"
    "    __int4b = %d\n"
    "    __int4c = %d\n"
    "    __int4d = %d\n"
    "    __int4e = %d\n"
    "}\n",
    anim_data->frame_total / 100, anim_data->frame_total,
    anim_data->flags[0],anim_data->flags[1],
    anim_data->data[0],
    anim_data->data[1],
    anim_data->data[2],
    anim_data->data[3],
    anim_data->data[4],
    anim_data->data[5],
    anim_data->data[6],
    anim_data->data[7],
    anim_data->data[8],
    anim_data->data[9],
    anim_data->data[10],
    anim_data->data[11],
    anim_data->data[12],
    anim_data->data[13],
    anim_data->data[14],
    anim_data->data[15],
    anim_data->data[16],
    anim_data->data[17],
    anim_data->data[18],
    anim_data->data[19],
    anim_data->data[20],
    anim_data->data[21],
    anim_data->data[22],
    anim_data->data[23],
    anim_data->col_count,
    anim_data->hurt_count,
    anim_data->hit_count,
    anim_data->__int4b,
    anim_data->__int4c,
    anim_data->__int4d,
    anim_data->__int4e
    );
}

void dump_boxdata(BoxData* box_data) {
    log_printf(
        "BoxData {\n"
        "   matrix : \n"
        "   [%3f,%3f,%3f,%3f]\n"
        "   [%3f,%3f,%3f,%3f]\n"
        "   [%3f,%3f,%3f,%3f]\n"
        "   [%3f,%3f,%3f,%3f]\n"
        "   width : %f\n"
        "   height : %f\n"
        "   length : %f\n"
        "   __float4c : %f\n"
        "   type : %d\n"
        "}\n",
        box_data->matrix[0][0],box_data->matrix[0][1],box_data->matrix[0][2],box_data->matrix[0][3],
        box_data->matrix[1][0],box_data->matrix[1][1],box_data->matrix[1][2],box_data->matrix[1][3],
        box_data->matrix[2][0],box_data->matrix[2][1],box_data->matrix[2][2],box_data->matrix[2][3],
        box_data->matrix[3][0],box_data->matrix[3][1],box_data->matrix[3][2],box_data->matrix[3][3],
        box_data->width,
        box_data->height,
        box_data->length,
        box_data->__float4c,
        box_data->type
    );    
}

void dump_takedata(TakeData* take) {
    log_printf(
    "attackLV:%d\n"
    "cancelLV:%d\n"
    "__int24:%d\n"
    "__bool25:%d\n",
    take->LVs[0],
    take->LVs[1],
    take->__int24, 
    take->__bool25
    );    
}

int debug(ManbowActor2D* player) {
    if (!player)return 0;
    std::shared_ptr<ManbowAnimationController2D> cont = player->anim_controller;
    TakeData* take_data = cont->take;
    AnimationData* anim_data = cont->animation_data;
    log_printf("motion: %d take: %d frame: %d\n", cont->motion, cont->key_take, cont->key_frame);

    // if (cont->take)dump_takedata(cont->take);
    // if(anim_data->hit_count)dump_boxdata(&anim_data->box_data[anim_data->hit_count -1]);
    // dump_framedata(anim_data);

    // int32_t i = 0;
    // while(cont->animation_data[i].frame_total > 0 && cont->animation_data[i].frame_total % 100 == 0) {
    //     dump_framedata(&cont->animation_data[i++]);
    // }

    // FILE *out;
    // out = fopen("flag_dump.txt","a");
    // log_fprintf(out,"%d,",cont->animation_data->flags[0]);
    // fclose(out);

    // if(anim_data->__int4b != 0)log_printf("__int4b : %d\n", anim_data->__int4b);
    // log_printf("0x%p\n", cont->__unk140);
    // void** data = (void**)cont->__unk140;
    // if (data){
    //     log_printf("dump {\n");
    //     void** ptr = data;
    //     for (size_t i = 0; i <= 48; ++i){
    //         void** d = (ptr + i);
    //         log_printf("+0x%x = 0x%p\n",i*4,*d);
    //     }
    //     log_printf("}\n");
    // }
    return 0;
}

static forceinline void clip_to_screen(float* dst, const float* src) {
    dst[0] = (1.0f + src[0]) * (1280.0f / 2.0f);
    dst[1] = (1.0f - src[1]) * (720.0f / 2.0f);
}

static forceinline vec<float, 4> clip_to_screen(vec<float, 4> src) {
    vec<float, 4> one = { 1.0f, 1.0f, 1.0f, 1.0f };
    vec<float, 4> src_signed = { src[0], -src[1], src[2], -src[3] };
    vec<float, 4> mult = { 1280.0f / 2.0f, 720.0f / 2.0f, 1280.0f / 2.0f, 720.0f / 2.0f };
    return (one + src_signed) * mult;
}

static forceinline float vec2_distance(const float* p0, const float* p1) {
    float x = p0[0] - p1[0];
    float y = p0[1] - p1[1];
    return sqrtf(x * x + y * y);
}

static forceinline float vec4_distance(vec<float, 4> p) {
    float x = p[0] - p[2];
    float y = p[1] - p[3];
    return sqrtf(x * x + y * y);
}

static float hitbox_border_width = 0.0f;
static void draw_rect(uint32_t col, size_t index, const vec<float, 4>* points) {

    vec<float, 4> points_low = points[0];
    vec<float, 4> points_high = points[1];
    *(vec<float, 4>*)&hitbox_vertices[index * 4] = points_low;
    *(vec<float, 4>*)&hitbox_vertices[index * 4 + 2] = points_high;

    points_low = clip_to_screen(points_low);
    points_high = clip_to_screen(points_high);

    float width = vec4_distance(points_low);
    float height = vec4_distance(shufflevec(points_low, points_high, 0, 1, 6, 7));
    hitbox_gpu_data[index].thresholds[0] = 1.0f - hitbox_border_width / width;
    hitbox_gpu_data[index].thresholds[1] = 1.0f - hitbox_border_width / height;
    hitbox_gpu_data[index].col = col;
    hitbox_gpu_data[index].flags = 0;
}

static void draw_circle(uint32_t col, size_t index, vec<float, 4> aabb) {
    vec<float, 4> low_pos = shufflevec(aabb, aabb, 0, 1, 2, 1);
    *(vec<float, 4>*)&hitbox_vertices[index * 4] = low_pos;
    *(vec<float, 4>*)&hitbox_vertices[index * 4 + 2] = shufflevec(aabb, aabb, 2, 3, 0, 3);

    float threshold = 1.0f - hitbox_border_width / vec4_distance(clip_to_screen(low_pos));
    hitbox_gpu_data[index].thresholds[0] = threshold * threshold;
    hitbox_gpu_data[index].col = col;
    hitbox_gpu_data[index].flags = 1;
}

template <typename F> requires(std::is_invocable_v<F, const DrawObj&>)
static void draw_objs(const std::vector<DrawObj>& vec, F&& col_func) {
    size_t index = hitbox_write_idx;
    if (index != MAX_HITBOXES) {
        for (const auto& obj : vec) {
            uint32_t col = col_func(obj);

            switch (obj.prim) {
                case DRAW_RECT:
                    draw_rect(col, index, obj.rect.points_vec);
                    break;
                case DRAW_CIRCLE:
                    draw_circle(col, index, obj.circle.aabb_vec);
                    break;
            }

            if (++index == MAX_HITBOXES) {
                break;
            }
        }
        hitbox_write_idx = index;
    }
}

static void draw_objs(const std::vector<DrawObj>& vec, uint32_t col) {
    [[clang::always_inline]]
    draw_objs(vec, [=](const DrawObj&) -> uint32_t {
        return col;
    });
}

// ===========================================================================
// Native immediate-mode HUD (task #30 fix). See overlay.h for the rationale.
//
// A plugin queues text/rect commands via ::hud.* (native hud_text/hud_rect);
// overlay_draw expands the queue into the SAME hitbox quad pipeline (continuing
// hitbox_write_idx, sharing the one Map + DrawIndexed). Rollback-safe because
// the queue is a NATIVE std::vector on the DLL heap (exactly like the
// overlay_collision/hurt/hit vectors above — the render_arena hooks only reroute
// th155's OWN allocators, never the mingw CRT), the font is a static const
// array, and nothing here ever touches th155's String/DrawCommandSlot render
// path. Single render/sim thread (better_game_loop), so no locking is needed —
// hud_* (sim/Update) and overlay_draw (render) never overlap, same as the
// hitbox vectors.
// ===========================================================================
enum HudCmdType : uint8_t { HUD_TEXT, HUD_RECT };
struct HudCmd {
    uint8_t  type;
    float    x, y, w, h;
    float    scale;
    uint32_t rgba;      // 0xAARRGGBB
    char     text[128]; // HUD_TEXT only; NUL-terminated, truncated to fit
};
static std::vector<HudCmd> hud_cmds;

void hud_clear() {
    hud_cmds.clear();   // keeps capacity — no per-frame realloc churn
}

void hud_text(float x, float y, const char* str, uint32_t rgba, float scale) {
    if (!str) return;
    if (scale <= 0.0f) scale = 1.0f;
    HudCmd cmd;
    cmd.type = HUD_TEXT;
    cmd.x = x; cmd.y = y; cmd.w = 0.0f; cmd.h = 0.0f;
    cmd.scale = scale;
    cmd.rgba = rgba;
    size_t i = 0;
    for (; str[i] && i < sizeof(cmd.text) - 1; ++i) cmd.text[i] = str[i];
    cmd.text[i] = '\0';
    hud_cmds.push_back(cmd);
}

void hud_rect(float x, float y, float w, float h, uint32_t rgba) {
    HudCmd cmd;
    cmd.type = HUD_RECT;
    cmd.x = x; cmd.y = y; cmd.w = w; cmd.h = h;
    cmd.scale = 0.0f;
    cmd.rgba = rgba;
    cmd.text[0] = '\0';
    hud_cmds.push_back(cmd);
}

// Screen (1280x720, top-left origin) -> clip space [-1,1]. Inverse of the
// clip_to_screen() used by the hitbox path.
static forceinline float hud_scr_to_clip_x(float sx) { return sx * (2.0f / 1280.0f) - 1.0f; }
static forceinline float hud_scr_to_clip_y(float sy) { return 1.0f - sy * (2.0f / 720.0f); }

// Emit one FILLED, fully-solid rect into the hitbox buffers at slot `index`.
// Uses the rect (flags=0) shader path with thresholds forced to 0 so the frag
// shader's is_border is ALWAYS true -> it samples alphas[1] (== border_alpha).
// We pre-divide the packed alpha by border_alpha so the on-screen alpha equals
// the caller's alpha regardless of the hitbox border_alpha config (default 1.0).
static void hud_emit_rect(size_t index, float x, float y, float w, float h,
                          uint32_t rgba, float border_alpha) {
    float x0 = hud_scr_to_clip_x(x);
    float x1 = hud_scr_to_clip_x(x + w);
    float y0 = hud_scr_to_clip_y(y);
    float y1 = hud_scr_to_clip_y(y + h);
    HitboxVertex* vtx = &hitbox_vertices[index * 4];
    vtx[0].pos[0] = x0; vtx[0].pos[1] = y0;   // top-left
    vtx[1].pos[0] = x1; vtx[1].pos[1] = y0;   // top-right
    vtx[2].pos[0] = x1; vtx[2].pos[1] = y1;   // bottom-right
    vtx[3].pos[0] = x0; vtx[3].pos[1] = y1;   // bottom-left

    uint32_t a = (rgba >> 24) & 0xFF;
    if (border_alpha > 1.0f / 255.0f) {
        float ca = (float)a / border_alpha;
        a = ca >= 255.0f ? 255u : (uint32_t)(ca + 0.5f);
    }
    hitbox_gpu_data[index].col = (rgba & 0x00FFFFFFu) | (a << 24);
    hitbox_gpu_data[index].flags = 0;              // rect path (no circle)
    hitbox_gpu_data[index].thresholds[0] = 0.0f;   // abs(uv) >= 0 -> always "border"
    hitbox_gpu_data[index].thresholds[1] = 0.0f;
}

// Expand the queued HUD commands into hitbox quads, continuing hitbox_write_idx
// and capping at MAX_HITBOXES (graceful stop, never overflow). Called from
// overlay_draw AFTER the hitbox draw_objs, sharing the one Map + DrawIndexed.
static void hud_render_queue(float border_alpha) {
    size_t index = hitbox_write_idx;
    for (const HudCmd& cmd : hud_cmds) {
        if (index >= MAX_HITBOXES) break;
        if (cmd.type == HUD_RECT) {
            hud_emit_rect(index, cmd.x, cmd.y, cmd.w, cmd.h, cmd.rgba, border_alpha);
            ++index;
            continue;
        }
        // HUD_TEXT: monospace 8x8 font, one lit font-pixel = one scale*scale rect.
        float pen_x = cmd.x;
        for (const char* p = cmd.text; *p; ++p) {
            unsigned char ch = (unsigned char)*p;
            if (ch >= 0x20 && ch < 0x80) {
                const unsigned char* glyph = HUD_FONT8X8[ch];
                for (int row = 0; row < 8; ++row) {
                    unsigned char bits = glyph[row];
                    if (!bits) continue;
                    for (int col = 0; col < 8; ++col) {
                        if (!(bits & (1u << col))) continue;   // bit0 = leftmost
                        if (index >= MAX_HITBOXES) { hitbox_write_idx = index; return; }
                        hud_emit_rect(index,
                                      pen_x + (float)col * cmd.scale,
                                      cmd.y  + (float)row * cmd.scale,
                                      cmd.scale, cmd.scale,
                                      cmd.rgba, border_alpha);
                        ++index;
                    }
                }
            }
            pen_x += 8.0f * cmd.scale;   // advance one monospace cell
        }
    }
    hitbox_write_idx = index;
}

void overlay_clear() {
    overlay_collision.clear();
    overlay_hurt.clear();
    overlay_hit.clear();
}

static HitboxConstantBuffer hitbox_last_cb = {};
void overlay_draw() {
    { static bool o = false; if (!o) { o = true;
        log_printf("[#28tid] overlay_draw(render) thread tid=%lu\n",
                   GetCurrentThreadId()); } }
    d3d_probe_arm();   // [#28] one-shot D3D vtable probe (SQUIROLL_D3D_PROBE=1)

    // [#30] Emit the squiroll-owned plugin HUD slot FORWARD-ONLY on the render
    // thread. Must run BEFORE the hitbox early-returns below so the HUD draws
    // even when the debug hitbox overlay is off. No-op until render_slot::init()
    // (driven from the sim thread) has created the slot; empty when no plugin
    // Text is connected. This is the "connection half" that lets a plugin draw
    // under rollback without th155 ever walking a half-rewound connection list.
    render_slot::emit();

    if (!overlay_supported)
        return;

    bool has_collision = !overlay_collision.empty();
    bool has_hurt = !overlay_hurt.empty();
    bool has_hit = !overlay_hit.empty();
    bool has_hitboxes = has_collision || has_hurt || has_hit;
    bool has_hud = !hud_cmds.empty();
    // [#30] Also render when the native HUD queue is non-empty (the plugin HUD
    // draws even with the debug hitbox overlay off). (This also fixes the old
    // early-return that tested has_hurt twice instead of has_hit.)
    if (!has_hitboxes && !has_hud)
        return;

    hitbox_border_width = get_hitbox_border_width() * 2.0f;

    // Try not to read config entries if we don't have to
    uint32_t collision_col = has_collision ? get_hitbox_collision_color() : 0;
    uint32_t hit_col = has_hit ? get_hitbox_hit_color() : 0;
    uint32_t player_hurt_col = has_hurt ? get_hitbox_player_hurt_color() : 0;
    uint32_t player_unhit_col = has_hurt ? get_hitbox_player_unhit_color() : 0;
    uint32_t player_ungrab_col = has_hurt ? get_hitbox_player_ungrab_color() : 0;
    uint32_t player_unhit_ungrab_col = has_hurt ? get_hitbox_player_unhit_ungrab_color() : 0;
    uint32_t misc_hurt_col = has_hurt ? get_hitbox_misc_hurt_color() : 0;

    ID3D11DeviceContext* imm = *d3d11_imm;

    D3D11_MAPPED_SUBRESOURCE resource;
    HitboxConstantBuffer new_cb = {
        .alphas = {get_hitbox_inner_alpha(), get_hitbox_border_alpha()}
    };
    if (memcmp(&new_cb, &hitbox_last_cb, sizeof(HitboxConstantBuffer))) {
        hitbox_last_cb = new_cb;

        imm->Map(hitbox_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &resource);
        memcpy(resource.pData, &new_cb, sizeof(HitboxConstantBuffer));
        imm->Unmap(hitbox_cb, 0);
    }

    imm->Map(hitbox_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &resource);
    hitbox_vertices = (HitboxVertex*)resource.pData;
    imm->Map(hitbox_sb, 0, D3D11_MAP_WRITE_DISCARD, 0, &resource);
    hitbox_gpu_data = (HitboxGPUData*)resource.pData;
    hitbox_write_idx = 0;

    draw_objs(overlay_collision, collision_col);
    draw_objs(overlay_hurt, [=](const DrawObj& obj) -> uint32_t {
        if (obj.group & 3) {
            int flags = hitbox_player_flags[(obj.group & 2) != 0] & 5;
            switch (flags) {
                case 5:
                    return player_hurt_col;
                case 4:
                    return player_unhit_col;
                case 1:
                    return player_ungrab_col;
                case 0:
                    return player_unhit_ungrab_col;
                default:
                    unreachable;
            }
        } else {
            return misc_hurt_col;
        }
    });
    draw_objs(overlay_hit, hit_col);

    // [#30] Expand the native HUD queue into the SAME quad buffers, continuing
    // hitbox_write_idx. Pass the border alpha (alphas[1]) so hud_emit_rect can
    // compensate for the shader's alpha modulation and render fully solid.
    if (has_hud)
        hud_render_queue(new_cb.alphas[1]);

    imm->Unmap(hitbox_vb, 0);
    imm->Unmap(hitbox_sb, 0);

    ID3D11InputLayout* orig_il;
    D3D_PRIMITIVE_TOPOLOGY orig_topology;
    ID3D11VertexShader* orig_vs;
    ID3D11PixelShader* orig_ps;
    imm->IAGetInputLayout(&orig_il);
    imm->IAGetPrimitiveTopology(&orig_topology);
    imm->VSGetShader(&orig_vs, nullptr, 0);
    imm->PSGetShader(&orig_ps, nullptr, 0);

    static constexpr UINT stride = sizeof(HitboxVertex);
    static constexpr UINT offset = 0;
    imm->IASetInputLayout(hitbox_il);
    imm->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    imm->IASetVertexBuffers(0, 1, &hitbox_vb, &stride, &offset);
    imm->IASetIndexBuffer(hitbox_ib, DXGI_FORMAT_R16_UINT, 0);
    imm->VSSetShader(hitbox_vs, nullptr, 0);
    imm->VSSetShaderResources(0, 1, &hitbox_sb_srv);
    imm->PSSetShader(hitbox_ps, nullptr, 0);
    imm->PSSetConstantBuffers(0, 1, &hitbox_cb);

    imm->DrawIndexed(hitbox_write_idx * 6, 0, 0);

    static constexpr ID3D11ShaderResourceView* null_srv = nullptr;
    static constexpr ID3D11Buffer* null_buf = nullptr;
    imm->IASetInputLayout(orig_il);
    imm->IASetPrimitiveTopology(orig_topology);
    imm->IASetVertexBuffers(0, 1, &null_buf, &stride, &offset);
    imm->IASetIndexBuffer(null_buf, DXGI_FORMAT_R16_UINT, 0);
    imm->VSSetShader(orig_vs, nullptr, 0);
    imm->VSSetShaderResources(0, 1, &null_srv);
    imm->PSSetShader(orig_ps, nullptr, 0);
    imm->PSSetConstantBuffers(0, 1, &null_buf);
}

// ---------------------------------------------------------------------------
// GPU idle-sync before a rollback restore (task #28). The NVIDIA D3D11 driver
// processes the submitted command queue on its own worker threads AFTER
// Present returns; the next tick's rollback then rewrites tf4-mspace memory
// those threads still reference -> exec-at-heap crash on a driver thread.
// Flush the immediate context and spin on an EVENT query until the GPU has
// drained, so nothing is in-flight when the restore rewrites the arenas.
// Same thread as render (better_game_loop), so touching the immediate context
// here is safe.
void gpu_sync_idle() {
    ID3D11Device*        dev = d3d11_dev ? *d3d11_dev : nullptr;
    ID3D11DeviceContext* imm = d3d11_imm ? *d3d11_imm : nullptr;
    static int diag = 40;   // throttled diagnostics for the first calls
    if (!dev || !imm) {
        if (diag > 0) { --diag; log_printf("[gpusync] SKIP dev=%p imm=%p\n", dev, imm); }
        return;
    }
    static ID3D11Query* q = nullptr;
    if (!q) {
        D3D11_QUERY_DESC qd{};
        qd.Query = D3D11_QUERY_EVENT;
        if (FAILED(dev->CreateQuery(&qd, &q)) || !q) {
            if (diag > 0) { --diag; log_printf("[gpusync] CreateQuery FAILED\n"); }
            imm->Flush();
            return;
        }
    }
    imm->End(q);          // signals when the GPU reaches this point
    imm->Flush();         // kick the queue so the event can actually complete
    int spin = 0;
    HRESULT hr = S_FALSE;
    for (; spin < 8000000; ++spin) {
        BOOL done = FALSE;
        hr = imm->GetData(q, &done, sizeof(done), 0);
        if (hr == S_OK) break;      // GPU drained
        if (hr != S_FALSE) break;   // error — don't hang
    }
    if (diag > 0) { --diag; log_printf("[gpusync] drained spin=%d hr=0x%08x\n", spin, (unsigned)hr); }
}
